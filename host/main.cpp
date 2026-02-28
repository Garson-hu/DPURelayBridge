#include <iostream>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <getopt.h>

// #include <infiniband/verbs.h>
// #include <infiniband/mlx5dv.h>

#include "../common/protocol.h"
#include "../common/logger.h"

extern "C" {
    #include "../common/cgmk_legacy/cross_gvmi_mkey.h"
    #include "../common/cgmk_legacy/cgmk_utils.h"
}


/* Command line input structure:
 * - dst_ip: the IP address of the DPU
 * - ib_devname: the name of the IB device
 * - dst_port: the port of the DPU
 */
struct cli_input {
    char *dst_ip;
    char *ib_devname;
    uint16_t dst_port;
};

// -------------------------------------------------------------------
// Parse the command line parameters
// -------------------------------------------------------------------
static void usage(const char *argv0) {
    std::cout << "Usage:\n";
    std::cout << "  Host Agent prepares GVMI MKeys and shares them with local DPU.\n\n";
    std::cout << "Options:\n";
    std::cout << "  -a, --dst-ip-address=<192.168.100.2> (DPU Arm IP)\n";
    std::cout << "  -d, --device=<mlx5_2>                (Host IB Device)\n";
    std::cout << "  -p, --dst-port=<1234>                (DPU Port)\n";
}

static int parse_command_line(int argc, char *argv[], struct cli_input *usr_par) {
    if (argc <= 1) {
        usage(argv[0]);
        return 1;
    }
    memset(usr_par, 0, sizeof(*usr_par));
    while (1) {
        int c;
        static struct option long_options[] = {
            { "dst-ip-address", required_argument, 0, 'a' },
            { "device",         required_argument, 0, 'd' },
            { "dst-port",       required_argument, 0, 'p' },
            { 0, 0, 0, 0 }
        };
        c = getopt_long(argc, argv, "a:d:p:", long_options, NULL);
        if (c == -1) break;
        switch (c) {
            case 'a': usr_par->dst_ip = optarg; break;
            case 'd': usr_par->ib_devname = optarg; break;
            case 'p': usr_par->dst_port = strtoul(optarg, NULL, 0); break;
            default: usage(argv[0]); return 1;
        }
    }
    return 0;
}

// Helper function：Change the old cgmk_mr_export function to convert the old string to our new structure
void populate_mem_info(struct cgmk_mkey* mr, const char* token, BufferType type, HostMemInfo* out_info) {
    memset(out_info, 0, sizeof(HostMemInfo));
    out_info->type = type;
    
    // Write the exported string directly to the structure
    size_t desc_len = cgmk_mr_export(mr, (char*)token, strlen(token) + 1, out_info->desc_str, sizeof(out_info->desc_str));

    if (desc_len == 0) {
        SPDLOG_ERROR("Failed to export MR!");
        exit(1);
    }
}

int main(int argc, char *argv[]) {

    /* Initialize logger */
    init_logger();

    /* Parse command line */
    int ret;
    struct cli_input 		 usr_par;
    ret = parse_command_line(argc, argv, &usr_par);

    if(ret) {
        exit(1);
    }

    // * 16 MB buffer size for the buffer to be shared with the DPU
    size_t BUF_SIZE = 16 * 1024 * 1024;  

    SPDLOG_INFO("============================================");
    SPDLOG_INFO("  Host Agent Started  - Create GVMI MKey    ");
    SPDLOG_INFO("  Target IB Device : {}", usr_par.ib_devname);
    SPDLOG_INFO("  Target DPU IP    : {}:{}", usr_par.dst_ip, usr_par.dst_port);
    SPDLOG_INFO("  Buffer Size      : {} Bytes", BUF_SIZE);
    SPDLOG_INFO("============================================");

    // -------------------------------------------------------------------
    // Step A: Find and open IB device, allocate Protection Domain (PD)
    // -------------------------------------------------------------------
    SPDLOG_DEBUG("Initializing InfiniBand device...");

    struct ibv_device **dev_list = ibv_get_device_list(NULL);
    if (!dev_list) {
        SPDLOG_ERROR("Failed to get IB device list.");
        exit(EXIT_FAILURE);
    }

    /* Find IB device */
    struct ibv_device *ib_dev = NULL;
    for (int i = 0; dev_list[i]; ++i) {
        if (!strcmp(ibv_get_device_name(dev_list[i]), usr_par.ib_devname)) {
            ib_dev = dev_list[i];
            break;
        }
    }

    if (!ib_dev) {
        SPDLOG_ERROR("IB device {} not found.", usr_par.ib_devname);
        ibv_free_device_list(dev_list);
        exit(EXIT_FAILURE);
    }

    /* Open IB device */
    struct ibv_context *context = ibv_open_device(ib_dev);
    if (!context) {
        SPDLOG_ERROR("Failed to open IB device '{}'", usr_par.ib_devname);
        ibv_free_device_list(dev_list);
        exit(EXIT_FAILURE);
    }

    /* Allocate Protection Domain (PD) */
    struct ibv_pd *pd = ibv_alloc_pd(context);
    if (!pd) {
        SPDLOG_ERROR("Couldn't allocate PD.");
        exit(EXIT_FAILURE);
    }

    SPDLOG_INFO("InfiniBand device and PD initialized successfully.");
    

    // ------------------------------------------------------------------------------
    // Step B: Allocate memory and execute signature (Reuse the logic of cgmk/mk_sender.c)
    // ------------------------------------------------------------------------------ 

    SPDLOG_DEBUG("Allocating and signing Primary and Mirror buffers...");
    
    // Use page alignment to allocate memory, instead of malloc
    // void *primary_buf = nullptr;
    // void *mirror_buf  = nullptr;
    // posix_memalign(&primary_buf, sysconf(_SC_PAGESIZE), BUF_SIZE);
    // posix_memalign(&mirror_buf, sysconf(_SC_PAGESIZE), BUF_SIZE);
    
    void *primary_buf = malloc(BUF_SIZE);
    void *mirror_buf = malloc(BUF_SIZE);

    if (!primary_buf || !mirror_buf) {
        SPDLOG_ERROR("Failed to allocate aligned host memory.");
        exit(EXIT_FAILURE);
    }

    // ------------------------------------------------------------------------------
    // Step C: Register the underlying Cross-GVMI MKey for each of these two memory buffers
    // ------------------------------------------------------------------------------

    SPDLOG_DEBUG("Creating Cross-GVMI MKeys...");
    struct cgmk_mkey *primary_mr = create_cgmk_mkey(pd, primary_buf, BUF_SIZE);
    struct cgmk_mkey *mirror_mr  = create_cgmk_mkey(pd, mirror_buf, BUF_SIZE);

    if (!primary_mr || !mirror_mr) {
        SPDLOG_ERROR("Failed to create Cross-GVMI MKeys.");
        free(primary_buf);
        free(mirror_buf);
        exit(EXIT_FAILURE);
    }
    
    SPDLOG_INFO("Cross-GVMI MKeys created!");

    // ------------------------------------------------------------------------------
    // Step D: Export the MKey information as HostMemInfo structure
    // ------------------------------------------------------------------------------
    
    HostMemInfo primary_info = {0};
    HostMemInfo mirror_info = {0};

    // * The token is used to access the Cross-GVMI MKey for the remote machine
    const char* token = "eShVkYp3s6v9y$B&E)H@McQfTjWnZq4t";

    populate_mem_info(primary_mr, token, BUFFER_PRIMARY, &primary_info);
    populate_mem_info(mirror_mr,  token, BUFFER_MIRROR,  &mirror_info);

    SPDLOG_INFO("MKeys successfully exported!");

    // ---------------------------------------------------------
    // Step E: Connect to the local DPU and transfer control
    // ---------------------------------------------------------
    SPDLOG_DEBUG("Connecting to local DPU via TCP...");

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in r_addr;
    r_addr.sin_family = AF_INET;
    r_addr.sin_port = htons(usr_par.dst_port);
    r_addr.sin_addr.s_addr = inet_addr(usr_par.dst_ip);

    if (connect(sockfd, (struct sockaddr *)&r_addr, sizeof(r_addr)) < 0) {
        SPDLOG_ERROR("Socket connection to DPU {}:{} failed. Errno: {}", usr_par.dst_ip, usr_par.dst_port, errno);
        SPDLOG_ERROR("Please ensure DPU Receiver is running on the DPU Arm core.");
        exit(EXIT_FAILURE);
    }

    SPDLOG_INFO("Connected to DPU successfully.");

    // * Send the HostMemInfo structures to the DPU
    ssize_t ret1 = write(sockfd, &primary_info, sizeof(HostMemInfo));
    ssize_t ret2 = write(sockfd, &mirror_info, sizeof(HostMemInfo));

    if (ret1 != sizeof(HostMemInfo) || ret2 != sizeof(HostMemInfo)) {
        SPDLOG_ERROR("Failed to send HostMemInfo structures to DPU.");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    SPDLOG_INFO("Memory credentials successfully handed over to the DPU!");

    // ---------------------------------------------------------
    // Step F: Suspend and wait for the DPU's release signal (ACK)
    // ---------------------------------------------------------
    // Host CPU will go to sleep and no data copy will be performed.

    char ackmsg[256];
    ssize_t recv_bytes = recv(sockfd, ackmsg, sizeof(ackmsg), 0);
    
    if (recv_bytes <= 0) {
        SPDLOG_WARN("DPU closed the connection or error occurred. Host Agent shutting down.");
    } else {
        SPDLOG_INFO("Received completion signal from DPU. Shutting down cleanly.");
    }

    // ---------------------------------------------------------
    // Step G: release resources
    // ---------------------------------------------------------
    close(sockfd);
    dereg_cgmk_mkey(primary_mr);
    dereg_cgmk_mkey(mirror_mr);
    free(primary_buf);
    free(mirror_buf);
    ibv_dealloc_pd(pd);
    ibv_close_device(context);
    ibv_free_device_list(dev_list);

    SPDLOG_INFO("Host Agent exited.");

    return 0;
}