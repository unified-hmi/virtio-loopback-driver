# Virtio loopback driver

> This is a linux kernel virtio loopback driver.  It provides a kernel API to create
> virtio devices that are emulated in user space.

## Introduction

The driver is used by the [Unified HMI](https://github.com/unified-hmi) project to provide remote GPU implementation.
The main repository for Unified HMI project sources is [`remote-virtio-gpu`](https://github.com/unified-hmi/remote-virtio-gpu).

## Build instructions

Use this command to build the kernel module:

```
make -C "/lib/modules/$(uname -r)/build" M="$(pwd)" modules
```

Use this command to cleanup:

```
make -C "/lib/modules/$(uname -r)/build" M="$(pwd)" modules
```
