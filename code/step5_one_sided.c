#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <infiniband/verbs.h>

#define BUFFER_SIZE 1024
#define TCP_PORT    19876

struct conn_data {
    uint32_t qp_num;
    uint16_t lid;
    uint8_t  gid[16];
    uint32_t rkey;
    uint64_t addr;
};

// ---------------------------------------------------------------------------
// QP State Transitions
// ---------------------------------------------------------------------------

static int modify_qp_to_init(struct ibv_qp *qp, uint8_t port_num) {
    struct ibv_qp_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.qp_state        = IBV_QPS_INIT;
    attr.port_num        = port_num;
    attr.pkey_index      = 0;
    attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE |
                           IBV_ACCESS_REMOTE_READ |
                           IBV_ACCESS_REMOTE_WRITE;
    return ibv_modify_qp(qp, &attr,
        IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);
}

static int modify_qp_to_rtr(struct ibv_qp *qp, struct conn_data *remote,
                             uint8_t port_num, int use_gid, int gid_idx) {
    struct ibv_qp_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.qp_state           = IBV_QPS_RTR;
    attr.path_mtu           = IBV_MTU_1024;
    attr.dest_qp_num        = remote->qp_num;
    attr.rq_psn             = 0;
    attr.max_dest_rd_atomic = 1;
    attr.min_rnr_timer      = 12;
    attr.ah_attr.port_num      = port_num;
    attr.ah_attr.sl            = 0;
    attr.ah_attr.src_path_bits = 0;

    if (use_gid) {
        attr.ah_attr.is_global = 1;
        attr.ah_attr.dlid      = 0;
        memcpy(&attr.ah_attr.grh.dgid, remote->gid, 16);
        attr.ah_attr.grh.sgid_index    = gid_idx;
        attr.ah_attr.grh.hop_limit     = 1;
        attr.ah_attr.grh.flow_label    = 0;
        attr.ah_attr.grh.traffic_class = 0;
    } else {
        attr.ah_attr.is_global = 0;
        attr.ah_attr.dlid      = remote->lid;
    }

    return ibv_modify_qp(qp, &attr,
        IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
        IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER);
}

static int modify_qp_to_rts(struct ibv_qp *qp) {
    struct ibv_qp_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.qp_state      = IBV_QPS_RTS;
    attr.timeout       = 14;
    attr.retry_cnt     = 7;
    attr.rnr_retry     = 7;
    attr.sq_psn        = 0;
    attr.max_rd_atomic = 1;
    return ibv_modify_qp(qp, &attr,
        IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
        IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC);
}

// ---------------------------------------------------------------------------
// One-Sided Operations
// ---------------------------------------------------------------------------

static int post_rdma_write(struct ibv_qp *qp, struct ibv_mr *local_mr,
                           uint64_t remote_addr, uint32_t remote_rkey) {
    struct ibv_sge sge;
    struct ibv_send_wr wr, *bad_wr;

    sge.addr   = (uintptr_t)local_mr->addr;
    sge.length = local_mr->length;
    sge.lkey   = local_mr->lkey;

    memset(&wr, 0, sizeof(wr));
    wr.wr_id               = 200;
    wr.sg_list             = &sge;
    wr.num_sge             = 1;
    wr.opcode              = IBV_WR_RDMA_WRITE;
    wr.send_flags          = IBV_SEND_SIGNALED;
    wr.wr.rdma.remote_addr = remote_addr;
    wr.wr.rdma.rkey        = remote_rkey;

    return ibv_post_send(qp, &wr, &bad_wr);
}

static int post_rdma_read(struct ibv_qp *qp, struct ibv_mr *local_mr,
                          uint64_t remote_addr, uint32_t remote_rkey) {
    struct ibv_sge sge;
    struct ibv_send_wr wr, *bad_wr;

    sge.addr   = (uintptr_t)local_mr->addr;
    sge.length = local_mr->length;
    sge.lkey   = local_mr->lkey;

    memset(&wr, 0, sizeof(wr));
    wr.wr_id               = 201;
    wr.sg_list             = &sge;
    wr.num_sge             = 1;
    wr.opcode              = IBV_WR_RDMA_READ;
    wr.send_flags          = IBV_SEND_SIGNALED;
    wr.wr.rdma.remote_addr = remote_addr;
    wr.wr.rdma.rkey        = remote_rkey;

    return ibv_post_send(qp, &wr, &bad_wr);
}

static int poll_completion(struct ibv_cq *cq) {
    struct ibv_wc wc;
    int result;

    printf("[INFO] Polling CQ...\n");
    do {
        result = ibv_poll_cq(cq, 1, &wc);
    } while (result == 0);

    if (result < 0) {
        fprintf(stderr, "CQ poll failed.\n");
        return -1;
    }
    if (wc.status == IBV_WC_SUCCESS) {
        printf("[SUCCESS] Work Request %lu completed.\n", wc.wr_id);
        return 0;
    }
    fprintf(stderr, "[ERROR] WR %lu failed: %s\n",
            wc.wr_id, ibv_wc_status_str(wc.status));
    return -1;
}

// ---------------------------------------------------------------------------
// Out-of-Band TCP Exchange (kept open for synchronization)
// ---------------------------------------------------------------------------

static int tcp_server_setup(struct conn_data *local, struct conn_data *remote) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(TCP_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(sockfd);
        return -1;
    }
    listen(sockfd, 1);
    printf("[INFO] Waiting for client on TCP port %d...\n", TCP_PORT);

    int client_fd = accept(sockfd, NULL, NULL);
    close(sockfd);
    if (client_fd < 0) {
        perror("accept");
        return -1;
    }

    write(client_fd, local, sizeof(*local));
    read(client_fd, remote, sizeof(*remote));

    return client_fd;
}

static int tcp_client_setup(const char *server_ip,
                            struct conn_data *local, struct conn_data *remote) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(TCP_PORT);
    inet_pton(AF_INET, server_ip, &addr.sin_addr);

    printf("[INFO] Connecting to server %s:%d...\n", server_ip, TCP_PORT);
    if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(sockfd);
        return -1;
    }

    read(sockfd, remote, sizeof(*remote));
    write(sockfd, local, sizeof(*local));

    return sockfd;
}

static void tcp_barrier(int fd) {
    char c = 'S';
    write(fd, &c, 1);
    read(fd, &c, 1);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char *argv[]) {

    int is_server = (argc == 1);
    const char *server_ip = is_server ? NULL : argv[1];
    uint8_t port_num = 1;

    printf("=== RDMA One-Sided Write/Read ===\n");
    printf("Role: %s\n\n", is_server ? "TARGET (memory exposed)" : "INITIATOR (performs RDMA)");

    // -----------------------------------------------------------------------
    // Connection Setup Flow (see 02_README_INFINIBAND.md → Out-of-Band Exchange)
    //
    //  1. Local Initialization  — open device, allocate PD, register MR, create CQ & QP
    //  2. QP → INIT             — set port number and access permissions
    //  3. Pre-post Receives     — (2-sided only; skipped for 1-sided operations)
    //  4. Gather Local Info     — query local QP number, LID, GID
    //  5. Establish OOB Channel — open a TCP socket between the two nodes
    //  6. Data Swap             — exchange QPN, LID, GID, + rkey & remote addr
    //  7. QP → RTR              — configure remote QPN, LID, GID, PSN
    //  8. QP → RTS              — set timeout, retry count, send PSN
    //  9. OOB Teardown          — close TCP (or keep for sync barriers)
    // 10. Data Flow             — all transfers now go over InfiniBand
    // -----------------------------------------------------------------------

    // --- Step 1: Local Initialization (device discovery) ---
    struct ibv_device **dev_list = ibv_get_device_list(NULL);
    if (!dev_list) {
        fprintf(stderr, "No RDMA devices found.\n");
        return 1;
    }
    struct ibv_context *context = ibv_open_device(dev_list[0]);
    if (!context) {
        fprintf(stderr, "Failed to open device.\n");
        return 1;
    }
    printf("[INFO] Device: %s\n", ibv_get_device_name(dev_list[0]));

    struct ibv_port_attr port_attr;
    if (ibv_query_port(context, port_num, &port_attr)) {
        fprintf(stderr, "Failed to query port %d.\n", port_num);
        return 1;
    }

    int use_gid = (port_attr.link_layer == IBV_LINK_LAYER_ETHERNET);
    int gid_idx = use_gid ? 1 : 0;
    printf("[INFO] Link layer: %s\n",
           use_gid ? "Ethernet (RoCE/Soft-RoCE)" : "InfiniBand");

    // --- Step 1 (cont.): Resource Allocation (PD, MR, CQ, QP) ---
    struct ibv_pd *pd = ibv_alloc_pd(context);
    if (!pd) { fprintf(stderr, "Failed to allocate PD.\n"); return 1; }

    char *buffer = calloc(1, BUFFER_SIZE);
    struct ibv_mr *mr = ibv_reg_mr(pd, buffer, BUFFER_SIZE,
        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
    if (!mr) { fprintf(stderr, "Failed to register MR.\n"); return 1; }

    struct ibv_cq *cq = ibv_create_cq(context, 10, NULL, NULL, 0);
    if (!cq) { fprintf(stderr, "Failed to create CQ.\n"); return 1; }

    struct ibv_qp_init_attr qp_init_attr;
    memset(&qp_init_attr, 0, sizeof(qp_init_attr));
    qp_init_attr.send_cq          = cq;
    qp_init_attr.recv_cq          = cq;
    qp_init_attr.qp_type          = IBV_QPT_RC;
    qp_init_attr.cap.max_send_wr  = 10;
    qp_init_attr.cap.max_recv_wr  = 10;
    qp_init_attr.cap.max_send_sge = 1;
    qp_init_attr.cap.max_recv_sge = 1;

    struct ibv_qp *qp = ibv_create_qp(pd, &qp_init_attr);
    if (!qp) { fprintf(stderr, "Failed to create QP.\n"); return 1; }
    printf("[INFO] Resources allocated (PD, MR, CQ, QP:%u)\n", qp->qp_num);

    // Target pre-fills its buffer so the RDMA Write visibly overwrites it
    if (is_server) {
        strncpy(buffer, "Original server memory content", BUFFER_SIZE - 1);
        printf("[INFO] Buffer initialized: \"%s\"\n", buffer);
    }

    // --- Step 2: QP → INIT ---
    // (Step 3 is skipped — no pre-posted Receive WQEs needed for 1-sided operations)
    if (modify_qp_to_init(qp, port_num)) {
        fprintf(stderr, "Failed: QP -> INIT\n");
        return 1;
    }

    // --- Steps 4-6: Gather local info, establish OOB channel, exchange data ---
    // For 1-sided operations, rkey and remote address are exchanged IN ADDITION
    // to the baseline QPN/LID/GID, so the initiator can access remote memory directly.
    struct conn_data local_data, remote_data;
    memset(&local_data, 0, sizeof(local_data));
    local_data.qp_num = qp->qp_num;               // Step 4: query local QP number
    local_data.lid    = port_attr.lid;              // Step 4: query local LID
    local_data.rkey   = mr->rkey;                   // 1-sided extra: remote permission key
    local_data.addr   = (uint64_t)(uintptr_t)buffer; // 1-sided extra: remote virtual address

    if (use_gid) {                                  // Step 4: query local GID
        union ibv_gid my_gid;
        ibv_query_gid(context, port_num, gid_idx, &my_gid);
        memcpy(local_data.gid, &my_gid, 16);
    }

    // Steps 5-6: open TCP socket and swap connection data (kept open for barriers)
    int tcp_fd;
    if (is_server) {
        tcp_fd = tcp_server_setup(&local_data, &remote_data);
    } else {
        tcp_fd = tcp_client_setup(server_ip, &local_data, &remote_data);
    }
    if (tcp_fd < 0) return 1;

    printf("[INFO] Exchanged: Remote QPN=%u, Remote LID=%u, Remote rkey=%u\n",
           remote_data.qp_num, remote_data.lid, remote_data.rkey);

    // --- Step 7: QP → RTR (uses remote QPN, LID, GID from the exchange) ---
    if (modify_qp_to_rtr(qp, &remote_data, port_num, use_gid, gid_idx)) {
        fprintf(stderr, "Failed: QP -> RTR\n");
        return 1;
    }
    // --- Step 8: QP → RTS (set timeout, retry count, send PSN) ---
    if (modify_qp_to_rts(qp)) {
        fprintf(stderr, "Failed: QP -> RTS\n");
        return 1;
    }
    printf("[INFO] QP ready (INIT -> RTR -> RTS)\n");

    // --- Step 9: OOB channel kept open for TCP barriers (synchronization) ---

    // --- Step 10: Data Flow (all transfers over InfiniBand) ---
    if (is_server) {
        // TARGET: the CPU does nothing during the RDMA operations.
        // We use TCP barriers to know when to check the buffer.

        printf("\n--- Waiting for RDMA Write from initiator ---\n");
        tcp_barrier(tcp_fd);
        printf("[DATA] Buffer after RDMA Write: \"%s\"\n", buffer);
        printf("       (Modified silently — target CPU was never involved)\n");

        printf("\n--- Allowing initiator to perform RDMA Read ---\n");
        tcp_barrier(tcp_fd);
        printf("[INFO] Initiator has read our buffer via RDMA Read.\n");

    } else {
        // INITIATOR: performs RDMA Write, then RDMA Read.

        // -- RDMA Write: push local data into target's memory --
        const char *write_msg = "Overwritten by RDMA Write!";
        strncpy(buffer, write_msg, BUFFER_SIZE - 1);
        printf("\n--- RDMA Write ---\n");
        printf("[INFO] Writing \"%s\" into target's memory...\n", buffer);

        if (post_rdma_write(qp, mr, remote_data.addr, remote_data.rkey)) {
            fprintf(stderr, "Failed to post RDMA Write.\n");
            return 1;
        }
        if (poll_completion(cq)) return 1;
        tcp_barrier(tcp_fd);

        // -- RDMA Read: pull target's memory into local buffer --
        memset(buffer, 0, BUFFER_SIZE);
        printf("\n--- RDMA Read ---\n");
        printf("[INFO] Reading target's memory into local buffer...\n");

        if (post_rdma_read(qp, mr, remote_data.addr, remote_data.rkey)) {
            fprintf(stderr, "Failed to post RDMA Read.\n");
            return 1;
        }
        if (poll_completion(cq)) return 1;
        printf("[DATA] RDMA Read result: \"%s\"\n", buffer);
        printf("       (Pulled directly from target's RAM — target CPU unaware)\n");
        tcp_barrier(tcp_fd);
    }

    // --- Cleanup (close OOB, then reverse order of allocation) ---
    close(tcp_fd);
    ibv_destroy_qp(qp);
    ibv_destroy_cq(cq);
    ibv_dereg_mr(mr);
    free(buffer);
    ibv_dealloc_pd(pd);
    ibv_close_device(context);
    ibv_free_device_list(dev_list);

    printf("\n[DONE] Clean shutdown.\n");
    return 0;
}
