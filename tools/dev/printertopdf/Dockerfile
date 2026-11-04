# PrinterToPDF (RWAP Software, GPL-3.0, https://github.com/RWAP/PrinterToPDF):
# an ESC/P renderer the FX-80 renderer is compared with, on the Mac only. Its
# output is a second opinion, not the expected result, and none of its code
# goes into the firmware. Built as its own Dockerfile builds it, at a fixed
# commit; tools/dev/printertopdf.sh runs it.
FROM ubuntu:22.04

RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates wget unzip gcc make \
       libc6-dev libpng-dev libsdl1.2-dev libhpdf-dev \
    && rm -rf /var/lib/apt/lists/*

ARG COMMIT=feaba094ba79d08250d138ddc6702921099a26cf
RUN wget -q -O /tmp/src.zip "https://github.com/RWAP/PrinterToPDF/archive/${COMMIT}.zip" \
    && unzip -q /tmp/src.zip -d /opt \
    && mv "/opt/PrinterToPDF-${COMMIT}" /opt/printertopdf \
    && rm /tmp/src.zip \
    && make -C /opt/printertopdf

WORKDIR /opt/printertopdf
