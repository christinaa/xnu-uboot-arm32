# shitloader 

include $(TOPDIR)/config.mk

LIB	= $(obj)libbootkit.o

COBJS-y += ./mach-o/macho.o
COBJS-y += ./mach-o/macho_util.o

COBJS-y += ./serialize/jsmn.o
COBJS-y += ./serialize/xml_plist.o

COBJS-y += ./device_tree/device_tree.o

COBJS-y += ./main/strol.o
COBJS-y += ./main/memory.o
COBJS-y += ./main/loader.o
COBJS-y += ./main/mach_boot.o

COBJS-y += ./main/JS_device_tree.o
COBJS-y += ./main/XML_device_tree.o

COBJS-y += ./asn1/asn1.o

COBJS-y += ./image3/image3.o

COBJS-y += ./compressed/lzss.o
COBJS-y += ./compressed/quicklz.o

COBJS	:= $(COBJS-y)
SRCS	:= $(COBJS:.o=.c)
OBJS	:= $(addprefix $(obj),$(COBJS))

$(LIB):	$(obj).depend $(OBJS)
	$(call cmd_link_o_target, $(OBJS))

CFLAGS += -std=gnu99

# defines $(obj).depend target
include $(SRCTREE)/rules.mk

sinclude $(obj).depend
