#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <infiniband/verbs.h>

#define BUFFER_SIZE 1024

int main() {

    // Boilerplate from Program 1 to get context
    struct ibv_device **dev_list = ibv_get_device_list(NULL);
    if (!dev_list) return 1;
    struct ibv_context *context = ibv_open_device(dev_list[0]);
    if (!context) return 1;

    // 1. Allocate Protection Domain (PD)
    // The PD ensures that only authorized QPs can access specific Memory Regions
    struct ibv_pd *pd = ibv_alloc_pd(context);
    if (!pd) {
        fprintf(stderr, "Failed to allocate PD.\n");
        return 1;
    }
    printf("[INFO] Protection Domain allocated.\n");

    // 2. Allocate RAM and Register Memory Region (MR)
    // We must pin the memory so the hardware can access it via DMA
    char *buffer = malloc(BUFFER_SIZE);
    memset(buffer, 0, BUFFER_SIZE);

    struct ibv_mr *mr = ibv_reg_mr(
        pd,
        buffer,
        BUFFER_SIZE,
        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE
    );
    if (!mr) {
        fprintf(stderr, "Failed to register MR.\n");
        return 1;
    }
    printf("[INFO] Memory Region registered.\n");
    printf("       Local Key (lkey): %u\n", mr->lkey);
    printf("       Remote Key (rkey): %u\n", mr->rkey);  // We will send this to the peer later

    // 3. Create Completion Queue (CQ)
    // Hardware places notifications here when operations finish
    struct ibv_cq *cq = ibv_create_cq(context, 10, NULL, NULL, 0);
    if (!cq) {
        fprintf(stderr, "Failed to create CQ.\n");
        return 1;
    }
    printf("[INFO] Completion Queue created.\n");

    // 4. Create Queue Pair (QP)
    // This is the actual communication channel (Send Queue + Receive Queue)
    struct ibv_qp_init_attr qp_init_attr;
    memset(&qp_init_attr, 0, sizeof(qp_init_attr));
    qp_init_attr.send_cq = cq;
    qp_init_attr.recv_cq = cq;
    qp_init_attr.qp_type = IBV_QPT_RC; // Reliable Connection transport
    qp_init_attr.cap.max_send_wr = 10;
    qp_init_attr.cap.max_recv_wr = 10;
    qp_init_attr.cap.max_send_sge = 1;
    qp_init_attr.cap.max_recv_sge = 1;

    struct ibv_qp *qp = ibv_create_qp(pd, &qp_init_attr);
    if (!qp) {
        fprintf(stderr, "Failed to create QP.\n");
        return 1;
    }
    printf("[SUCCESS] Queue Pair created. QP Number: %u\n", qp->qp_num);

    // 5. Teardown (Always destroy in reverse order of creation)
    ibv_destroy_qp(qp);
    ibv_destroy_cq(cq);
    ibv_dereg_mr(mr);
    free(buffer);
    ibv_dealloc_pd(pd);
    ibv_close_device(context);
    ibv_free_device_list(dev_list);

    return 0;
}