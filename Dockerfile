FROM debian:13-slim

ENV DEBIAN_FRONTEND=noninteractive

ARG DEBIAN_MIRRORS="deb.debian.org"
ARG PKG_LISTS="ca-certificates build-essential libcurl4-openssl-dev uuid-dev libreadline-dev"

RUN sed -i "s/deb.debian.org/${DEBIAN_MIRRORS}/g" /etc/apt/sources.list.d/debian.sources && \
    sed -i "s/security.debian.org/${DEBIAN_MIRRORS}/g" /etc/apt/sources.list.d/debian.sources

RUN apt update && \
    apt install -y --no-install-recommends ${PKG_LISTS} && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /spinc
CMD ["/bin/bash"]