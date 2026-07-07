# LTP — LogLSM Transport Protocol

Version: draft v1  
Project: LogLSM

---

# 1. Overview

LTP (LogLSM Transport Protocol) is a lightweight binary transport protocol for embedded systems.

LTP is designed for:
- STM32 microcontrollers
- DMA-oriented UART communication
- USB CDC transport
- low-power systems
- stream-based packet transport
- modular embedded architectures

LTP is derived from:
- SLIP (Serial Line Internet Protocol)
- WAKE protocol concepts

LTP extends these ideas with:
- DMA-friendly architecture
- FSM parser design
- CRC16 integrity checking
- layered transport architecture
- extensible packet format

---

# 2. Design Goals

The protocol is optimized for:

- low RAM usage
- low CPU overhead
- continuous stream processing
- packet fragmentation tolerance
- DMA RX ring buffer architectures
- reliable parser synchronization
- embedded modularity
- service-oriented communication

LTP is NOT intended to be:
- TCP replacement
- network routing protocol
- high-level RPC framework

---

# 3. Architecture

LTP uses layered architecture.

## 3.1 Physical Layer

Responsible only for byte transport.

Supported transports:
- UART
- USB CDC
- RS-485
- BLE UART bridge (future)

The physical layer MUST NOT contain:
- parser logic
- packet logic
- application logic

---

## 3.2 Framing Layer

Uses SLIP-compatible framing.

### Constants

```c
#define LTP_FEND   0xC0
#define LTP_FESC   0xDB
#define LTP_TFEND  0xDC
#define LTP_TFESC  0xDD
```

### Escaping Rules

| Byte | Encoded Sequence |
|---|---|
| 0xC0 | 0xDB 0xDC |
| 0xDB | 0xDB 0xDD |

All packets MUST begin with:
```text
FEND
```

---

## 3.3 Transport Layer

Defines packet structure.

### Packet Format

```text
FEND | ADDR | CMD | FLAGS | SEQ | LEN | PAYLOAD | CRC16
```

---

# 4. CRC

LTP uses CRC16-CCITT.

Recommended parameters:

| Parameter | Value |
|---|---|
| Polynomial | 0x1021 |
| Init | 0xFFFF |
| Reflect In | false |
| Reflect Out | false |
| XOR Out | 0x0000 |

---

# 5. Parser Architecture

Recommended architecture:

```text
UART RX
    ↓
DMA
    ↓
Ring Buffer
    ↓
FSM Parser
    ↓
Dispatcher
    ↓
Services
```

---

# 6. FSM Parser

Recommended states:

- WAIT_FEND
- READ_HEADER
- READ_PAYLOAD
- READ_CRC
- ESCAPE
- PACKET_DONE
- ERROR

Parser requirements:
- tolerate fragmented input
- recover after corruption
- recover after CRC failure
- recover synchronization automatically

---

# 7. DMA-Oriented Design

Recommended model:

```text
UART -> DMA -> Circular Ring Buffer
```

Advantages:
- low CPU load
- low interrupt rate
- low power consumption
- continuous stream support

---

# 8. Layered Architecture

Layers:
- Physical Layer
- Framing Layer
- Transport Layer
- Parser Layer
- Dispatcher Layer
- Service Layer

---

# 9. Dispatcher

Dispatcher routes parsed packets to services.

Example:

| CMD | Service |
|---|---|
| 0x10 | LoggerService |
| 0x20 | ConfigService |
| 0x30 | SensorService |

---

# 10. Service Layer

Services implement business logic.

Examples:
- telemetry
- logging
- diagnostics
- firmware update
- configuration
- expansion modules

---

# 11. Error Recovery

On CRC failure:
- discard packet
- continue stream scanning
- search next FEND

---

# 12. Recommended Embedded Implementation

```text
USART RX DMA Circular Mode
↓
RX Ring Buffer
↓
FSM Stream Parser
↓
Packet Queue
↓
Dispatcher
↓
Services
```

---

# 13. Summary

LTP is evolution of:

```text
SLIP → WAKE → LTP
```

LTP combines:
- SLIP simplicity
- WAKE lightweight philosophy
- modern DMA/FSM architecture
- modular layered transport design
