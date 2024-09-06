FROM ubuntu:22.04

RUN apt-get update && apt-get install -y \
                    git \
                    wget \
                    flex \
                    bison \
                    gperf \
                    python3 \
                    python3-pip \
                    python3-venv \
                    python3-setuptools \
                    cmake \
                    ccache \
                    libffi-dev \
                    libssl-dev \
                    dfu-util \
                    libusb-1.0-0 \
                    libgcrypt-dev \
                    libglib2.0-dev \
                    libfdt-dev \
                    libpixman-1-dev \
                    zlib1g-dev \
                    ninja-build \
                    libslirp-dev



RUN mkdir -p ~/esp && cd ~/esp && git clone -b v4.4.7 --recursive https://github.com/espressif/esp-idf.git && cd esp-idf && ./install.sh && /bin/bash -c "source  ./export.sh"

RUN echo 'source /root/esp/esp-idf/export.sh' >> $HOME/.bashrc

RUN mkdir -p ~/qemu && cd ~/qemu && git clone --recursive https://github.com/espressif/qemu.git && cd qemu \
    && mkdir build && cd build \
    && ../configure  --target-list=xtensa-softmmu --enable-gcrypt --enable-slirp --enable-debug --enable-sanitizers --disable-sdl --disable-strip --disable-user --disable-capstone --disable-vnc --disable-gtk \
    && cd .. \
    && ninja -C build \
    && ninja -C build install

WORKDIR /root

ENV PROJECT_ROOT=/root/esp/esp_wireguard
ENV EXAMPLE_PATH=$PROJECT_ROOT/examples/demo_qemu

COPY . $PROJECT_ROOT

WORKDIR $EXAMPLE_PATH

RUN ["/bin/bash", "-c", "source /root/esp/esp-idf/export.sh && idf.py build && cd build && esptool.py --chip esp32 merge_bin --fill-flash-size 4MB -o flash_image.bin @flash_args"]

CMD ["/bin/bash", "-c", "/root/qemu/qemu/build/qemu-system-xtensa -nic user,model=open_eth,id=lo0 -no-reboot -nographic -machine esp32 -drive file=${EXAMPLE_PATH}/build/flash_image.bin,if=mtd,format=raw"]
