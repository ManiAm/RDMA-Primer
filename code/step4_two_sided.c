#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <infiniband/verbs.h>

#define BUFFER_SIZE 1024
#define TCP_PORT    19875

struct conn_data {
    uint32_t qp_num;
    uint16_t lid;
    uint8_t  gid[16];
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
// Two-Sided Operations
// ---------------------------------------------------------------------------

static int post_receive(struct ibv_qp *qp, struct ibv_mr *mr) {
    struct ibv_sge sge;
    struct ibv_recv_wr wr, *bad_wr;

    sge.addr   = (uintptr_t)mr->addr;
    sge.length = mr->length;
    sge.lkey   = mr->lkey;

    memset(&wr, 0, sizeof(wr));
    wr.wr_id   = 101;
    wr.sg_list = &sge;
    wr.num_sge = 1;

    return ibv_post_recv(qp, &wr, &bad_wr);
}

static int post_send(struct ibv_qp *qp, struct ibv_mr *mr) {
    struct ibv_sge sge;
    struct ibv_send_wr wr, *bad_wr;

    sge.addr   = (uintptr_t)mr->addr;
    sge.length = mr->length;
    sge.lkey   = mr->lkey;

    memset(&wr, 0, sizeof(wr));
    wr.wr_id      = 100;
    wr.sg_list    = &sge;
    wr.num_sge    = 1;
    wr.opcode     = IBV_WR_SEND;
    wr.send_flags = IBV_SEND_SIGNALED;

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
// Out-of-Band TCP Exchange
// ---------------------------------------------------------------------------

static int tcp_server_exchange(struct conn_data *local, struct conn_data *remote) {
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
    if (client_fd < 0) {
        perror("accept");
        close(sockfd);
        return -1;
    }

    write(client_fd, local, sizeof(*local));
    read(client_fd, remote, sizeof(*remote));

    close(client_fd);
    close(sockfd);
    return 0;
}

static int tcp_client_exchange(const char *server_ip,
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

    close(sockfd);
    return 0;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char *argv[]) {

    int is_server = (argc == 1);
    const char *server_ip = is_server ? NULL : argv[1];
    uint8_t port_num = 1;

    printf("=== RDMA Two-Sided Send/Receive ===\n");
    printf("Role: %s\n\n", is_server ? "SERVER (receiver)" : "CLIENT (sender)");

    // -----------------------------------------------------------------------
    // Connection Setup Flow (see 02_README_INFINIBAND.md → Out-of-Band Exchange)
    //
    //  1. Local Initialization  — open device, allocate PD, register MR, create CQ & QP
    //  2. QP → INIT             — set port number and access permissions
    //  3. Pre-post Receives     — (2-sided only) post Receive WQEs before RTR
    //  4. Gather Local Info     — query local QP number, LID, GID
    //  5. Establish OOB Channel — open a TCP socket between the two nodes
    //  6. Data Swap             — exchange QPN, LID, GID (+ rkey/addr for 1-sided)
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

    // --- Step 2: QP → INIT ---
    if (modify_qp_to_init(qp, port_num)) {
        fprintf(stderr, "Failed: QP -> INIT\n");
        return 1;
    }

    // --- Step 3: Pre-post Receives (2-sided only) ---
    // The receiver posts Receive WQEs now, while the QP is in INIT.
    // Buffers must be ready before the QP reaches RTR, or incoming data has nowhere to land.
    if (is_server) {
        if (post_receive(qp, mr)) {
            fprintf(stderr, "Failed to post receive.\n");
            return 1;
        }
        printf("[INFO] Receive buffer posted.\n");
    }

    // --- Steps 4-6: Gather local info, establish OOB channel, exchange data ---
    // For 2-sided operations, only baseline data is needed: QPN, LID, GID.
    struct conn_data local_data, remote_data;
    memset(&local_data, 0, sizeof(local_data));
    local_data.qp_num = qp->qp_num;  // Step 4: query local QP number
    local_data.lid    = port_attr.lid; // Step 4: query local LID

    if (use_gid) {                     // Step 4: query local GID
        union ibv_gid my_gid;
        ibv_query_gid(context, port_num, gid_idx, &my_gid);
        memcpy(local_data.gid, &my_gid, 16);
    }

    // Steps 5-6: open TCP socket and swap connection data
    if (is_server) {
        if (tcp_server_exchange(&local_data, &remote_data)) return 1;
    } else {
        if (tcp_client_exchange(server_ip, &local_data, &remote_data)) return 1;
    }
    printf("[INFO] Exchanged: Remote QPN=%u, Remote LID=%u\n",
           remote_data.qp_num, remote_data.lid);

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

    // --- Step 9: OOB channel already closed by tcp_*_exchange() ---

    // --- Step 10: Data Flow (all transfers over InfiniBand) ---
    if (is_server) {
        printf("[INFO] Waiting for incoming message...\n");
        if (poll_completion(cq)) return 1;
        printf("\n  >>> Received: \"%s\"\n\n", buffer);
    } else {
        const char *msg = "Hello from RDMA client!";
        strncpy(buffer, msg, BUFFER_SIZE - 1);
        printf("[INFO] Sending: \"%s\"\n", buffer);
        if (post_send(qp, mr)) {
            fprintf(stderr, "Failed to post send.\n");
            return 1;
        }
        if (poll_completion(cq)) return 1;
    }

    // --- Cleanup (reverse order of allocation) ---
    ibv_destroy_qp(qp);
    ibv_destroy_cq(cq);
    ibv_dereg_mr(mr);
    free(buffer);
    ibv_dealloc_pd(pd);
    ibv_close_device(context);
    ibv_free_device_list(dev_list);

    printf("[DONE] Clean shutdown.\n");
    return 0;
}
