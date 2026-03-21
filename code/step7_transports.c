#include <stdio.h>
#include <string.h>
#include <infiniband/verbs.h>

int main() {

    struct ibv_device **dev_list = ibv_get_device_list(NULL);
    struct ibv_context *context = ibv_open_device(dev_list[0]);
    struct ibv_pd *pd = ibv_alloc_pd(context);
    struct ibv_cq *cq = ibv_create_cq(context, 10, NULL, NULL, 0);

    // -------------------------------------------------------------------
    // 1. CREATE A UD (Unreliable Datagram) QUEUE PAIR
    // -------------------------------------------------------------------
    struct ibv_qp_init_attr qp_init_attr;
    memset(&qp_init_attr, 0, sizeof(qp_init_attr));
    qp_init_attr.send_cq = cq;
    qp_init_attr.recv_cq = cq;

    // Changing this defines your Transport Service!
    // Options: IBV_QPT_RC (Reliable), IBV_QPT_UC (Unreliable), IBV_QPT_UD (Datagram)
    qp_init_attr.qp_type = IBV_QPT_UD;

    qp_init_attr.cap.max_send_wr = 10;
    qp_init_attr.cap.max_recv_wr = 10;
    qp_init_attr.cap.max_send_sge = 1;
    qp_init_attr.cap.max_recv_sge = 1;

    struct ibv_qp *qp = ibv_create_qp(pd, &qp_init_attr);
    if (!qp) {
        fprintf(stderr, "Failed to create UD QP.\n");
        return 1;
    }
    printf("Successfully created UD Queue Pair.\n");

    // -------------------------------------------------------------------
    // 2. CREATE AN ADDRESS HANDLE (AH) FOR UD ROUTING
    // Because UD doesn't have a 1-to-1 connection state, you must
    // explicitly create a "routing label" to attach to your sends.
    // -------------------------------------------------------------------
    struct ibv_ah_attr ah_attr;
    memset(&ah_attr, 0, sizeof(ah_attr));

    ah_attr.is_global     = 0; // 0 means we are on the same subnet
    ah_attr.dlid          = 5; // Simulated Destination Local ID (from peer)
    ah_attr.sl            = 0; // Service Level
    ah_attr.src_path_bits = 0;
    ah_attr.port_num      = 1; // Our local port

    struct ibv_ah *ah = ibv_create_ah(pd, &ah_attr);
    if (!ah) {
        fprintf(stderr, "Failed to create Address Handle.\n");
        return 1;
    }

    // -------------------------------------------------------------------
    // 3. POST A SEND USING UD (Requires the AH)
    // -------------------------------------------------------------------
    struct ibv_sge sge;
    struct ibv_send_wr wr, *bad_wr;

    memset(&sge, 0, sizeof(sge)); // In reality, map this to your MR
    memset(&wr, 0, sizeof(wr));

    wr.wr_id      = 401;
    wr.sg_list    = &sge;
    wr.num_sge    = 1;
    wr.opcode     = IBV_WR_SEND; // UD ONLY supports Send/Recv. No RDMA Read/Write!
    wr.send_flags = IBV_SEND_SIGNALED;

    // UD Specific Routing Requirements
    wr.wr.ud.ah          = ah;   // Attach the Address Handle
    wr.wr.ud.remote_qpn  = 1234; // Simulated Remote QP Number
    wr.wr.ud.remote_qkey = 0x11111111; // Q_Key (Queue Key for UD security)

    // Note: If you actually execute this without a valid MR, it will fail,
    // but this shows the exact struct setup.
    printf("UD Send Work Request prepared with Address Handle.\n");

    // Cleanup
    ibv_destroy_ah(ah);
    ibv_destroy_qp(qp);
    ibv_destroy_cq(cq);
    ibv_dealloc_pd(pd);
    ibv_close_device(context);
    ibv_free_device_list(dev_list);

    return 0;
}