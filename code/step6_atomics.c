#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <infiniband/verbs.h>

#define TCP_PORT 19877

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
                           IBV_ACCESS_REMOTE_WRITE |
                           IBV_ACCESS_REMOTE_ATOMIC;
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
// Atomic Operations
// ---------------------------------------------------------------------------

static int post_fetch_and_add(struct ibv_qp *qp, struct ibv_mr *local_mr,
                              uint64_t remote_addr, uint32_t remote_rkey,
                              uint64_t add_val) {
    struct ibv_sge sge;
    struct ibv_send_wr wr, *bad_wr;

    sge.addr   = (uintptr_t)local_mr->addr;
    sge.length = 8;
    sge.lkey   = local_mr->lkey;

    memset(&wr, 0, sizeof(wr));
    wr.wr_id                 = 302;
    wr.sg_list               = &sge;
    wr.num_sge               = 1;
    wr.opcode                = IBV_WR_ATOMIC_FETCH_AND_ADD;
    wr.send_flags            = IBV_SEND_SIGNALED;
    wr.wr.atomic.remote_addr = remote_addr;
    wr.wr.atomic.rkey        = remote_rkey;
    wr.wr.atomic.compare_add = add_val;

    return ibv_post_send(qp, &wr, &bad_wr);
}

static int post_compare_and_swap(struct ibv_qp *qp, struct ibv_mr *local_mr,
                                 uint64_t remote_addr, uint32_t remote_rkey,
                                 uint64_t expected, uint64_t swap_val) {
    struct ibv_sge sge;
    struct ibv_send_wr wr, *bad_wr;

    sge.addr   = (uintptr_t)local_mr->addr;
    sge.length = 8;
    sge.lkey   = local_mr->lkey;

    memset(&wr, 0, sizeof(wr));
    wr.wr_id                 = 301;
    wr.sg_list               = &sge;
    wr.num_sge               = 1;
    wr.opcode                = IBV_WR_ATOMIC_CMP_AND_SWP;
    wr.send_flags            = IBV_SEND_SIGNALED;
    wr.wr.atomic.remote_addr = remote_addr;
    wr.wr.atomic.rkey        = remote_rkey;
    wr.wr.atomic.compare_add = expected;
    wr.wr.atomic.swap        = swap_val;

    return ibv_post_send(qp, &wr, &bad_wr);
}

static int poll_completion(struct ibv_cq *cq) {
    struct ibv_wc wc;
    int result;

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

    printf("=== RDMA Atomic Operations ===\n");
    printf("Role: %s\n\n", is_server ? "TARGET (memory exposed)" : "INITIATOR (performs atomics)");
    printf("Usage:\n");
    printf("  Target (start first):  ./step6_atomics\n");
    printf("  Initiator:             ./step6_atomics <target_ip>\n\n");

    // --- 1. Device Discovery ---
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

    // --- 2. Resource Allocation ---
    // Atomic operations require 8-byte aligned buffers operating on uint64_t.
    struct ibv_pd *pd = ibv_alloc_pd(context);
    if (!pd) { fprintf(stderr, "Failed to allocate PD.\n"); return 1; }

    uint64_t *atomic_buf = aligned_alloc(8, sizeof(uint64_t));
    if (!atomic_buf) { fprintf(stderr, "Failed to allocate buffer.\n"); return 1; }
    *atomic_buf = 0;

    struct ibv_mr *mr = ibv_reg_mr(pd, atomic_buf, sizeof(uint64_t),
        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
        IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_ATOMIC);
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

    // Target initializes the shared counter
    if (is_server) {
        *atomic_buf = 1000;
        printf("[INFO] Atomic counter initialized: %lu\n", *atomic_buf);
    }

    // --- 3. QP -> INIT ---
    if (modify_qp_to_init(qp, port_num)) {
        fprintf(stderr, "Failed: QP -> INIT\n");
        return 1;
    }

    // --- 4. Exchange Connection Data via TCP ---
    struct conn_data local_data, remote_data;
    memset(&local_data, 0, sizeof(local_data));
    local_data.qp_num = qp->qp_num;
    local_data.lid    = port_attr.lid;
    local_data.rkey   = mr->rkey;
    local_data.addr   = (uint64_t)(uintptr_t)atomic_buf;

    if (use_gid) {
        union ibv_gid my_gid;
        ibv_query_gid(context, port_num, gid_idx, &my_gid);
        memcpy(local_data.gid, &my_gid, 16);
    }

    int tcp_fd;
    if (is_server) {
        tcp_fd = tcp_server_setup(&local_data, &remote_data);
    } else {
        tcp_fd = tcp_client_setup(server_ip, &local_data, &remote_data);
    }
    if (tcp_fd < 0) return 1;

    printf("[INFO] Exchanged: Remote QPN=%u, Remote rkey=%u\n",
           remote_data.qp_num, remote_data.rkey);

    // --- 5. QP -> RTR -> RTS ---
    if (modify_qp_to_rtr(qp, &remote_data, port_num, use_gid, gid_idx)) {
        fprintf(stderr, "Failed: QP -> RTR\n");
        return 1;
    }
    if (modify_qp_to_rts(qp)) {
        fprintf(stderr, "Failed: QP -> RTS\n");
        return 1;
    }
    printf("[INFO] QP ready (INIT -> RTR -> RTS)\n");

    // --- 6. Atomic Operations ---
    if (is_server) {

        // --- Fetch-and-Add ---
        printf("\n--- Waiting for Fetch-and-Add ---\n");
        tcp_barrier(tcp_fd);
        printf("[DATA] Counter after Fetch-and-Add: %lu\n", *atomic_buf);
        printf("       (Modified atomically by NIC — target CPU was not involved)\n");

        // --- Compare-and-Swap (success) ---
        printf("\n--- Waiting for Compare-and-Swap ---\n");
        tcp_barrier(tcp_fd);
        printf("[DATA] Counter after CAS: %lu\n", *atomic_buf);

        // --- Compare-and-Swap (failure) ---
        printf("\n--- Waiting for Compare-and-Swap (expected to fail) ---\n");
        tcp_barrier(tcp_fd);
        printf("[DATA] Counter after failed CAS: %lu  (unchanged)\n", *atomic_buf);

    } else {

        // --- Fetch-and-Add: add 10 to remote counter (1000 -> 1010) ---
        printf("\n--- Fetch-and-Add: adding 10 to remote counter ---\n");
        *atomic_buf = 0;
        if (post_fetch_and_add(qp, mr, remote_data.addr, remote_data.rkey, 10)) {
            fprintf(stderr, "Failed to post Fetch-and-Add.\n");
            return 1;
        }
        if (poll_completion(cq)) return 1;
        printf("[DATA] Original value returned: %lu  (remote is now %lu)\n",
               *atomic_buf, *atomic_buf + 10);
        tcp_barrier(tcp_fd);

        // --- Compare-and-Swap: if remote == 1010, swap to 9999 ---
        printf("\n--- Compare-and-Swap: expecting 1010, swapping to 9999 ---\n");
        *atomic_buf = 0;
        if (post_compare_and_swap(qp, mr, remote_data.addr, remote_data.rkey, 1010, 9999)) {
            fprintf(stderr, "Failed to post CAS.\n");
            return 1;
        }
        if (poll_completion(cq)) return 1;
        printf("[DATA] Original value returned: %lu\n", *atomic_buf);
        if (*atomic_buf == 1010)
            printf("       CAS succeeded (value matched, swapped to 9999)\n");
        else
            printf("       CAS failed (value was %lu, not 1010)\n", *atomic_buf);
        tcp_barrier(tcp_fd);

        // --- Compare-and-Swap (failure): expect 1010 again, but value is now 9999 ---
        printf("\n--- Compare-and-Swap: expecting 1010 (should FAIL, value is 9999) ---\n");
        *atomic_buf = 0;
        if (post_compare_and_swap(qp, mr, remote_data.addr, remote_data.rkey, 1010, 5555)) {
            fprintf(stderr, "Failed to post CAS.\n");
            return 1;
        }
        if (poll_completion(cq)) return 1;
        printf("[DATA] Original value returned: %lu\n", *atomic_buf);
        if (*atomic_buf == 1010)
            printf("       CAS succeeded (unexpected!)\n");
        else
            printf("       CAS failed as expected (value was %lu, not 1010 — no swap)\n", *atomic_buf);
        tcp_barrier(tcp_fd);
    }

    // --- 7. Cleanup ---
    close(tcp_fd);
    ibv_destroy_qp(qp);
    ibv_destroy_cq(cq);
    ibv_dereg_mr(mr);
    free(atomic_buf);
    ibv_dealloc_pd(pd);
    ibv_close_device(context);
    ibv_free_device_list(dev_list);

    printf("\n[DONE] Clean shutdown.\n");
    return 0;
}
