FROM espressif/idf:v6.1

# Non-root user so build output on the host isn't owned by root.
# The dev container tooling remaps its UID/GID to the host user's on Linux.
ARG USERNAME=dev
RUN (userdel -r ubuntu 2>/dev/null || true) \
    && useradd -m -u 1000 -s /bin/bash -G dialout ${USERNAME}

# ESP-IDF lives in /opt/esp and is owned by root
RUN git config --system --add safe.directory '*' \
    && echo '. "$IDF_PATH/export.sh" > /dev/null 2>&1' >> /etc/bash.bashrc
