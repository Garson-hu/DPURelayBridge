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

#define BUF_SIZE 16 * 1024 * 1024
// #define BUF_SIZE 24000

/* Command line input structure:
 * - dst_ip: the IP address of the DPU
 * - ib_devname: the name of the IB device
 * - dst_port: the port of the DPU
 * - is_server: True if this Host acts as TCP server for OOB
 * - remote_host_ip: Remote Host IP (needed if is_server == false)
 * - remote_host_port: Port for Host-to-Host communication
 */
struct cli_input {
    char *dst_ip;
    char *ib_devname;
    uint16_t dst_port;

    // * Fields for Host-to-Host OOB exchange
    bool is_server;         
    char *remote_host_ip;   
    uint16_t remote_host_port; 
};

// -------------------------------------------------------------------
// Parse the command line parameters
// -------------------------------------------------------------------
static void usage(const char *argv0) {
    std::cout << "Usage:\n";
    std::cout << "  Host Agent prepares GVMI MKeys and handles OOB exchange.\n\n";
    std::cout << "Options:\n";
    std::cout << "  -a, --dst-ip-address=<192.168.100.2> (Local DPU Arm IP)\n";
    std::cout << "  -d, --device=<mlx5_2>                (Local Host IB Device)\n";
    std::cout << "  -p, --dst-port=<1234>                (Local DPU Port)\n";
    std::cout << "  -s, --server                         (Run Host as OOB Server)\n";
    std::cout << "  -c, --client                         (Run Host as OOB Client)\n";
    std::cout << "  -R, --remote-host-ip=<IP>            (Remote Host IP, required for client)\n";
    std::cout << "  -P, --remote-host-port=<9999>        (Remote Host Port for OOB)\n";
}

static int parse_command_line(int argc, char *argv[], struct cli_input *usr_par) {
    if (argc <= 1) {
        usage(argv[0]);
        return 1;
    }
    memset(usr_par, 0, sizeof(*usr_par));
    usr_par->remote_host_port = 9999; // Default Host-to-Host port
    
    while (1) {
        int c;
        static struct option long_options[] = {
            { "dst-ip-address",   required_argument, 0, 'a' },
            { "device",           required_argument, 0, 'd' },
            { "dst-port",         required_argument, 0, 'p' },
            { "server",           no_argument,       0, 's' },
            { "client",           no_argument,       0, 'c' },
            { "remote-host-ip",   required_argument, 0, 'R' },
            { "remote-host-port", required_argument, 0, 'P' },
            { 0, 0, 0, 0 }
        };
        c = getopt_long(argc, argv, "a:d:p:scR:P:", long_options, NULL);
        if (c == -1) break;
        switch (c) {
            case 'a': usr_par->dst_ip = optarg; break;
            case 'd': usr_par->ib_devname = optarg; break;
            case 'p': usr_par->dst_port = strtoul(optarg, NULL, 0); break;
            case 's': usr_par->is_server = true; break;
            case 'c': usr_par->is_server = false; break;
            case 'R': usr_par->remote_host_ip = optarg; break;
            case 'P': usr_par->remote_host_port = strtoul(optarg, NULL, 0); break;
            default: usage(argv[0]); return 1;
        }
    }
    return 0;
}

// Helper function：Change the old cgmk_mr_export function to convert the old string to our new structure
void populate_mem_info(struct cgmk_mkey* mr, const char* token, BufferType type, HostMemInfo* out_info) {
    memset(out_info, 0, sizeof(HostMemInfo));
    out_info->type = type;

    size_t desc_len = cgmk_mr_export(mr, (char*)token, 32,
                                     out_info->desc_str,
                                     sizeof(out_info->desc_str));

    if (desc_len == 0 || desc_len >= sizeof(out_info->desc_str)) {
        SPDLOG_ERROR("Failed to export MR or descriptor too long!");
        exit(1);
    }

    out_info->desc_str[desc_len] = '\0';
    out_info->desc_str[sizeof(out_info->desc_str) - 1] = '\0';

    SPDLOG_DEBUG("exported desc_len = {}", desc_len);
    SPDLOG_DEBUG("Raw MR Descriptor string: {}", out_info->desc_str);
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

    SPDLOG_DEBUG("Allocating Primary and Mirror buffers...");
    
    // Use page alignment to allocate memory, instead of malloc
    void *primary_buf = nullptr;
    void *mirror_buf  = nullptr;
    int rc1 = posix_memalign(&primary_buf, sysconf(_SC_PAGESIZE), BUF_SIZE);
    int rc2 = posix_memalign(&mirror_buf, sysconf(_SC_PAGESIZE), BUF_SIZE);

    if (rc1 != 0 || rc2 != 0 || !primary_buf || !mirror_buf) {
        SPDLOG_ERROR("posix_memalign failed!");
        exit(EXIT_FAILURE);
    }

    // void *primary_buf = malloc(BUF_SIZE);
    // void *mirror_buf = malloc(BUF_SIZE);

    // int retp = sign_buffer(primary_buf, BUF_SIZE);
    // int retm = sign_buffer(mirror_buf, BUF_SIZE);

    // if (retp != 0 || retm != 0) {
    //     SPDLOG_ERROR("Failed to sign buffers.");
    //     exit(EXIT_FAILURE);
    // }

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
    SPDLOG_INFO("sizeof(HostMemInfo) = {}", sizeof(HostMemInfo));
    SPDLOG_INFO("sizeof(desc_str) = {}", strlen(primary_info.desc_str));
    // ---------------------------------------------------------
    // Step E: Connect to the local DPU and transfer control
    // ---------------------------------------------------------
    SPDLOG_DEBUG("Connecting to local DPU via TCP...");

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in r_addr = {};
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
    // Step F: Receive ACK from local DPU
    // ---------------------------------------------------------
    char ackmsg[3] = {0};
    ssize_t recv_bytes = recv(sockfd, ackmsg, 2, MSG_WAITALL); // Expecting "OK"
    
    if (recv_bytes <= 0) {
        SPDLOG_ERROR("DPU closed connection unexpectedly.");
        exit(EXIT_FAILURE);
    }
    SPDLOG_INFO("Received completion signal from DPU: {}", ackmsg);

    // ---------------------------------------------------------
    // Step G: Receive Network Credentials from local DPU
    // ---------------------------------------------------------
    SPDLOG_DEBUG("Waiting for Local DPU Network Credentials...");
    DpuRdmaInfo local_dpu_info = {};
    
    if (recv(sockfd, &local_dpu_info, sizeof(DpuRdmaInfo), MSG_WAITALL) != sizeof(DpuRdmaInfo)) {
        SPDLOG_ERROR("Failed to receive DpuRdmaInfo from local DPU.");
        exit(EXIT_FAILURE);
    }
    
    SPDLOG_INFO("Successfully received credentials from Local DPU (QPN: {})", (uint32_t)local_dpu_info.qp_num);
    
    // ---------------------------------------------------------
    // Step H: Host-to-Host Out-of-Band Exchange
    // ---------------------------------------------------------
    SPDLOG_INFO("Starting Host-to-Host OOB exchange on port {}...", usr_par.remote_host_port);
    DpuRdmaInfo remote_dpu_info = {};
    
    int host_sock = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(host_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    if (usr_par.is_server) {
        // Run as Server: Wait for Remote Host to connect
        struct sockaddr_in serv_addr = {};
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_addr.s_addr = INADDR_ANY;
        serv_addr.sin_port = htons(usr_par.remote_host_port);
        
        bind(host_sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr));
        listen(host_sock, 1);
        SPDLOG_INFO("Host is running as SERVER. Waiting for remote Host...");
        
        int conn_sock = accept(host_sock, NULL, NULL);
        if (conn_sock < 0) {
            SPDLOG_ERROR("Host Server accept failed.");
            exit(EXIT_FAILURE);
        }
        
        // Server: Recv then Send
        recv(conn_sock, &remote_dpu_info, sizeof(DpuRdmaInfo), MSG_WAITALL);
        send(conn_sock, &local_dpu_info, sizeof(DpuRdmaInfo), 0);
        close(conn_sock);
        
    } else {
        // Run as Client: Connect to Remote Host
        struct sockaddr_in serv_addr = {};
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port = htons(usr_par.remote_host_port);
        inet_pton(AF_INET, usr_par.remote_host_ip, &serv_addr.sin_addr);
        
        SPDLOG_INFO("Host is running as CLIENT. Connecting to remote Host {}...", usr_par.remote_host_ip);
        
        // Basic retry loop for client connection
        while (connect(host_sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
            SPDLOG_WARN("Waiting for remote Host server to be ready...");
            sleep(1);
        }
        
        // Client: Send then Recv
        send(host_sock, &local_dpu_info, sizeof(DpuRdmaInfo), 0);
        recv(host_sock, &remote_dpu_info, sizeof(DpuRdmaInfo), MSG_WAITALL);
    }
    close(host_sock);
    
    SPDLOG_INFO("Host-to-Host Exchange Complete! Received remote DPU credentials (QPN: {}).", (uint32_t)remote_dpu_info.qp_num);

    // ---------------------------------------------------------
    // Step I: Send Remote DPU Credentials down to Local DPU
    // ---------------------------------------------------------
    SPDLOG_DEBUG("Forwarding remote credentials to local DPU...");
    
    if (send(sockfd, &remote_dpu_info, sizeof(DpuRdmaInfo), 0) != sizeof(DpuRdmaInfo)) {
        SPDLOG_ERROR("Failed to send remote DPU credentials to local DPU.");
        exit(EXIT_FAILURE);
    }
    
    SPDLOG_INFO("Remote credentials forwarded. DPU highway is fully established!");

    // ---------------------------------------------------------
    // Phase 4: Data Plane Execution and Verification
    // ---------------------------------------------------------
    if (usr_par.is_server) {
        // Host A (Sender): Generate test data and trigger DPU A
        SPDLOG_INFO("[Sender]: Preparing test payload in Primary Buffer...");
        
        // Fill 16MB primary_buf with character 'A'
        memset(primary_buf, 'A', BUF_SIZE); 
        
        SPDLOG_INFO("[Sender]: Sending START signal to local DPU...");
        DataSyncMsg sync_msg = {SYNC_START, BUF_SIZE};
        send(sockfd, &sync_msg, sizeof(DataSyncMsg), 0);
        
        // Wait for DPU A to confirm it has pulled the data and sent it out
        recv(sockfd, &sync_msg, sizeof(DataSyncMsg), MSG_WAITALL);
        if (sync_msg.op == SYNC_DONE) {
            SPDLOG_INFO("[Sender]: Local DPU reported transmission complete.");
        }
        
    } else {
        // Host B (Receiver): Wait for DPU B to push data into mirror_buf
        SPDLOG_INFO("[Receiver]: Waiting for DPU to push data and send DONE signal...");
        
        DataSyncMsg sync_msg = {};
        ssize_t ret = recv(sockfd, &sync_msg, sizeof(DataSyncMsg), MSG_WAITALL);
        
        if (ret == sizeof(DataSyncMsg) && sync_msg.op == SYNC_DONE) {
            SPDLOG_INFO("[Receiver]: DONE signal received. Verifying Mirror Buffer...");
            
            char* ptr = (char*)mirror_buf;
            bool verification_passed = true;
            
            // Verify if all 16MB bytes are 'A'
            for (size_t i = 0; i < BUF_SIZE; i++) {
                if (ptr[i] != 'A') {
                    verification_passed = false;
                    SPDLOG_ERROR("Data mismatch at offset {}. Expected 'A', got 0x{:02x}", i, ptr[i]);
                    break;
                }
            }
            
            if (verification_passed) {
                SPDLOG_INFO("[Receiver]: SUCCESS! 16MB payload verified perfectly.");
            } else {
                SPDLOG_ERROR("[Receiver]: FAILURE! Payload verification failed.");
            }
        } else {
            SPDLOG_ERROR("[Receiver]: Connection lost or invalid sync message.");
        }
    }

    // ---------------------------------------------------------
    // Step G: Release resources
    // ---------------------------------------------------------
    SPDLOG_INFO("Cleaning up Host resources and shutting down...");
    close(sockfd);
    dereg_cgmk_mkey(primary_mr);
    dereg_cgmk_mkey(mirror_mr);
    free(primary_buf);
    free(mirror_buf);
    ibv_dealloc_pd(pd);
    ibv_close_device(context);
    ibv_free_device_list(dev_list);

    SPDLOG_INFO("Host Agent exited gracefully.");
    
    // ---------------------------------------------------------
    // Release resources
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