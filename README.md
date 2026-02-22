# DpuRelayBridge

## Design Goals
This project builds a clean, layered bridge between:

- **Local Host memory access**: make Host memory available to the local DPU through GVMI related metadata and access credentials.
- **Remote DPU to DPU transport**: move data between DPUs efficiently using RDMA.

The design keeps responsibilities separated so that:
- low level hardware operations stay isolated
- networking logic stays isolated
- the relay logic remains clear and testable

---

## High Level Architecture
The system has two planes:

### Control Plane
Used to exchange metadata and connection info.
- **Host ↔ DPU (local communication)**: Host provides the DPU with metadata to access Host memory regions.
- **DPU ↔ DPU (remote communication)**: DPUs exchange RDMA endpoint information required to connect QPs.

### Data Plane
Used to transfer payload data.
- The DPU reads from a **GVMI aliased Host memory region**.
- The DPU writes into a **remote mirror region** through an RDMA QP.

---

## Repository Layout
```
.
├── CMakeLists.txt
├── common
│   ├── logger.h
│   └── protocol.h
├── dpu
│   ├── CMakeLists.txt
│   ├── gvmi_wrapper.c
│   ├── host_controller.cpp
│   ├── main.cpp
│   ├── rdma_link.cpp
│   └── relay_engine.cpp
├── host
│   ├── CMakeLists.txt
│   ├── main.cpp
│   └── memory_manager.cpp
├── README.md
└── third_party
```


---

## Component Summary

### `common/`
Shared code used by both Host and DPU binaries.

- `protocol.h`  
  Defines the shared metadata formats exchanged over TCP.
  This covers:
  - Host memory description for local DPU consumption
  - DPU endpoint description for peer DPU connectivity

- `logger.h`  
  Lightweight logging utilities shared across modules.

---

### `host/` (Runs on x86 Host)
This side is responsible for making Host memory accessible to the local DPU.

Main responsibilities:
- allocate the memory regions required by the workflow
- prepare the metadata and access credentials needed for GVMI access
- serve these metadata to the local DPU over a simple local control channel

Files:
- `main.cpp`  
  Host program entry point. Coordinates memory setup and exposes metadata to the DPU.

- `memory_manager.cpp`  
  Host memory management wrapper. Provides a clean interface for:
  - allocating buffers
  - registering buffers for DPU access
  - exporting the information needed by the DPU

Design intent:
- keep all low level complexity contained
- keep the Host application logic small and stable

---

### `dpu/` (Runs on DPU Arm)
This side contains the core system logic. It talks to the local Host, connects to the peer DPU, and performs data movement.

Main responsibilities:
- fetch Host memory metadata from the local Host
- create local handles that can access Host memory via GVMI aliasing
- establish RDMA connectivity to the peer DPU
- run the relay logic to move data from local Host memory to remote Host mirror memory

Files:
- `main.cpp`  
  DPU program entry point. Bootstraps all subsystems and starts the relay.

- `host_controller.cpp`  
  Control plane client for the local Host. Receives Host memory metadata.

- `gvmi_wrapper.c`  
  GVMI facing wrapper. Converts Host provided metadata into a local representation usable by the DPU.

- `rdma_link.cpp`  
  RDMA connectivity module. Responsible for creating and connecting QPs to a peer DPU.

- `relay_engine.cpp`  
  The orchestrator. Implements the data movement schedule using:
  - a GVMI aliased local source region
  - an RDMA connection to the peer DPU for remote writes

---

### `third_party/`
Optional third party dependencies.
Examples:
- argument parsing
- logging libraries
- small utility headers

This directory keeps external code isolated from core logic.

---

## Build and Ownership Boundaries
- `host/` and `dpu/` build into two separate executables.
- `common/` is a shared library or shared include directory.
- Low level vendor specific operations are isolated in `memory_manager.cpp` and `gvmi_wrapper.c`.
- RDMA connection state and verbs logic are isolated in `rdma_link.cpp`.
- End to end orchestration is isolated in `relay_engine.cpp`.

---

## Extensibility Notes
This layout is designed to support future extensions without cross contaminating modules, such as:
- adding more memory regions or region types
- supporting multiple peer DPUs
- plugging in different control plane transports
- adding scheduling policies in the relay engine