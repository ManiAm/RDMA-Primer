#include <stdio.h>
#include <infiniband/verbs.h>

int main() {

    struct ibv_device **dev_list;
    struct ibv_context *context;
    struct ibv_port_attr port_attr;
    int num_devices;
    uint8_t port_num = 1; // ConnectX-4 usually uses port 1

    // 1. Get the list of all RDMA-capable devices
    dev_list = ibv_get_device_list(&num_devices);
    if (!dev_list || num_devices == 0) {
        fprintf(stderr, "Error: No RDMA devices found.\n");
        return 1;
    }
    printf("[INFO] Found %d RDMA device(s).\n", num_devices);

    for (int i = 0; i < num_devices; i++) {
        printf("  [%d] %s\n", i, ibv_get_device_name(dev_list[i]));
    }

    // 2. Open the first device to create a 'context'
    // This bypasses the OS kernel and gives us direct hardware access
    context = ibv_open_device(dev_list[0]);
    if (!context) {
        fprintf(stderr, "Error: Failed to open device context.\n");
        ibv_free_device_list(dev_list);
        return 1;
    }
    printf("[INFO] Opened device: %s\n", ibv_get_device_name(dev_list[0]));

    // 3. Query the physical port to ensure the cable is connected
    if (ibv_query_port(context, port_num, &port_attr)) {
        fprintf(stderr, "Error: Failed to query port %d.\n", port_num);
        return 1;
    }

    // 4. Verify the link is ACTIVE
    if (port_attr.state == IBV_PORT_ACTIVE) {
        printf("[SUCCESS] Port %d is ACTIVE and ready for traffic.\n", port_num);
    } else {
        printf("[WARNING] Port %d is NOT active. Check your point-to-point cable.\n", port_num);
    }

    // 5. Clean up
    ibv_close_device(context);
    ibv_free_device_list(dev_list);

    return 0;
}