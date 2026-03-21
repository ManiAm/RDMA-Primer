#include <stdio.h>
#include <string.h>
#include <infiniband/verbs.h>

// Helper to transition QP to INIT state
int modify_qp_to_init(struct ibv_qp *qp, uint8_t port_num) {
    struct ibv_qp_attr attr;
    memset(&attr, 0, sizeof(attr));

    attr.qp_state        = IBV_QPS_INIT;
    attr.port_num        = port_num;
    attr.pkey_index      = 0;
    attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;

    int flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;
    return ibv_modify_qp(qp, &attr, flags);
}

// Helper to transition QP to RTR (Ready to Receive) state
// Requires the destination (peer) QP number and LID (Local ID)
int modify_qp_to_rtr(struct ibv_qp *qp, uint32_t dest_qp_num, uint16_t dest_lid, uint8_t port_num) {
    struct ibv_qp_attr attr;
    memset(&attr, 0, sizeof(attr));

    attr.qp_state              = IBV_QPS_RTR;
    attr.path_mtu              = IBV_MTU_1024;
    attr.dest_qp_num           = dest_qp_num;
    attr.rq_psn                = 0; // Packet Sequence Number
    attr.max_dest_rd_atomic    = 1;
    attr.min_rnr_timer         = 12; // Receiver Not Ready timer
    attr.ah_attr.is_global     = 0;  // 0 means same subnet (no IP routing needed)
    attr.ah_attr.dlid          = dest_lid;
    attr.ah_attr.sl            = 0;  // Service Level
    attr.ah_attr.src_path_bits = 0;
    attr.ah_attr.port_num      = port_num;

    int flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
                IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
    return ibv_modify_qp(qp, &attr, flags);
}

// Helper to transition QP to RTS (Ready to Send) state
int modify_qp_to_rts(struct ibv_qp *qp) {
    struct ibv_qp_attr attr;
    memset(&attr, 0, sizeof(attr));

    attr.qp_state      = IBV_QPS_RTS;
    attr.timeout       = 14;
    attr.retry_cnt     = 7;
    attr.rnr_retry     = 7;
    attr.sq_psn        = 0;
    attr.max_rd_atomic = 1;

    int flags = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;
    return ibv_modify_qp(qp, &attr, flags);
}

int main() {

    // Boilerplate context/PD/CQ/QP creation
    struct ibv_device **dev_list = ibv_get_device_list(NULL);
    struct ibv_context *context = ibv_open_device(dev_list[0]);
    struct ibv_pd *pd = ibv_alloc_pd(context);
    struct ibv_cq *cq = ibv_create_cq(context, 10, NULL, NULL, 0);

    struct ibv_qp_init_attr qp_init_attr;
    memset(&qp_init_attr, 0, sizeof(qp_init_attr));
    qp_init_attr.send_cq = cq;
    qp_init_attr.recv_cq = cq;
    qp_init_attr.qp_type = IBV_QPT_RC;
    qp_init_attr.cap.max_send_wr = 1;
    qp_init_attr.cap.max_recv_wr = 1;
    qp_init_attr.cap.max_send_sge = 1;
    qp_init_attr.cap.max_recv_sge = 1;
    struct ibv_qp *qp = ibv_create_qp(pd, &qp_init_attr);

    uint8_t port_num = 1;

    // --- STATE MACHINE EXECUTION ---

    // 1. Transition to INIT
    if (modify_qp_to_init(qp, port_num)) {
        fprintf(stderr, "Failed to modify QP to INIT\n");
        return 1;
    }
    printf("[INFO] QP transitioned to INIT.\n");

    // 2. Simulate Out-of-Band TCP Exchange
    // In a real program, you open a TCP socket here, send your `qp->qp_num` and `lid`
    // to the remote machine, and receive theirs.
    uint32_t remote_qp_num = 1234; // Simulated received data
    uint16_t remote_lid = 5;       // Simulated received data
    printf("[INFO] Exchanged data via TCP. Remote QPN: %u, Remote LID: %u\n", remote_qp_num, remote_lid);

    // 3. Transition to RTR (Using the data from the peer)
    if (modify_qp_to_rtr(qp, remote_qp_num, remote_lid, port_num)) {
        fprintf(stderr, "Failed to modify QP to RTR\n");
        return 1;
    }
    printf("[INFO] QP transitioned to RTR. (Ready to Receive)\n");

    // 4. Transition to RTS
    if (modify_qp_to_rts(qp)) {
        fprintf(stderr, "Failed to modify QP to RTS\n");
        return 1;
    }
    printf("[SUCCESS] QP transitioned to RTS. (Ready to Send)\n");

    // Teardown
    ibv_destroy_qp(qp);
    ibv_destroy_cq(cq);
    ibv_dealloc_pd(pd);
    ibv_close_device(context);
    ibv_free_device_list(dev_list);

    return 0;
}