# Virtio loopback driver

> This is a linux kernel virtio loopback driver.  It provides a kernel API to create
> virtio devices that are emulated in user space.

## Introduction

The driver is used by the [Unified HMI](https://github.com/unified-hmi) project to provide remote GPU implementation.
The main repository for Unified HMI project sources is [`remote-virtio-gpu`](https://github.com/unified-hmi/remote-virtio-gpu).

## How to Install
You have two options for installing Virtio-loopback-driver: either build from source code ([Building from Source](#building-from-source)) or install from a binary ([Binary-only Install](#building-from-source)).

### Building from Source
Use this command to build the kernel module:

```
git clone https://github.com/unified-hmi/virtio-loopback-driver.git
cd ./virtio-loopback-driver
make -C "/lib/modules/$(uname -r)/build" M="$(pwd)" modules
sudo cp virtio-lo.ko /lib/modules/$(uname -r)/kernel/drivers/ 
sudo depmod
```

Use this command to cleanup:

```
make -C "/lib/modules/$(uname -r)/build" M="$(pwd)" clean
```
### Binary-only Install

  Download the `virtio-lo-dkms_X.X.X_amd64.deb`
  [DKMS](https://en.wikipedia.org/wiki/Dynamic_Kernel_Module_Support)
  ubuntu package from the latest release builds from its github repository and install it.
  Specify the correct version of `X.X.X` by referring to https://github.com/unified-hmi/virtio-loopback-driver/releases/latest  
  For example: `virtio-lo-dkms_1.0.0_amd64.deb`.  

 **Note:** An error about unresolved dependencies will occur when executing the following 2nd command, `sudo dpkg -i virtio-lo-dkms_X.X.X_amd64.deb`. This error can be resolved by running the 3rd one, `sudo apt -f install`.

  ```
  wget https://github.com/unified-hmi/virtio-loopback-driver/releases/latest/download/virtio-lo-dkms_X.X.X_amd64.deb 
  sudo dpkg -i virtio-lo-dkms_X.X.X_amd64.deb
  sudo apt -f install
  ```

