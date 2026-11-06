# The PDF volume (rp/src/pdfvol) built on this computer, for the host test.
PDFVOL := ../../rp/src/pdfvol
BUILD := ../dev/builds/pdfvol
CFLAGS := -std=c11 -O2 -Wall -Wextra -Werror -I$(PDFVOL)

$(BUILD)/volume: volume.c $(PDFVOL)/pdfvol.c $(PDFVOL)/pdfvol.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ volume.c $(PDFVOL)/pdfvol.c
