
# LogLSM / LTP Project Structure

## Core Architecture

Application Services
↓
LTP Core
↓
Transport Adapter
↓
USB CDC / UART / CAN FD

---

# Recommended Project Layout

```text
/Core
    /Inc
        ltp.h
        ltp_config.h
        ltp_crc.h
        ltp_parser.h
        ltp_transport.h

    /Src
        ltp.c
        ltp_crc.c
        ltp_parser.c
        ltp_transport_usb.c
        ltp_transport_uart.c
        ltp_transport_can.c

/Services
    log_service.c
    config_service.c
    ota_service.c

/Bootloader
    bl_ltp.c

/QtHost
    ltp_parser.cpp
    ltp_serializer.cpp
```

---

# Transport Strategy

## Primary Transport
- USB CDC Virtual COM

## Secondary Transport
- UART

## Future Transport
- CAN FD

---

# Design Principles

- No dynamic allocation
- DMA-friendly
- Ring buffer based
- Transport independent
- Packet oriented
- Embedded optimized
