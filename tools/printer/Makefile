# The FX-80 renderer (rp/src/fx80) built on this computer, for the host test.
FX80 := ../../rp/src/fx80
SRC := ../../rp/src
BUILD := ../dev/builds/printer
CFLAGS := -std=c11 -O2 -Wall -Wextra -Werror -I$(FX80) -I$(SRC)

$(BUILD)/render: render.c $(SRC)/pcl/pcl.c $(SRC)/pcl/pcl.h $(SRC)/pcl/pclpage.c $(SRC)/pcl/pclpage.h $(SRC)/pdf/pdf.c $(SRC)/pdf/pdf.h $(SRC)/pdf/deflate.c $(SRC)/pdf/deflate.h $(FX80)/fx80.c $(FX80)/raster.c $(FX80)/font.c $(FX80)/fx80.h $(FX80)/raster.h $(SRC)/md5/md5.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ render.c $(SRC)/pcl/pcl.c $(SRC)/pcl/pclpage.c $(SRC)/pdf/pdf.c $(SRC)/pdf/deflate.c $(FX80)/fx80.c $(FX80)/raster.c $(FX80)/font.c $(SRC)/md5/md5.c -lz

$(FX80)/font.c: $(FX80)/font.txt font.py
	./font.py c $@
