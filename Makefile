obj-m:=tcp_china.o

all:
	make -C /lib/modules/`uname -r`/build M=`pwd` modules CC=/usr/bin/gcc-4.9

clean:
	make -C /lib/modules/`uname -r`/build M=`pwd` clean

install:
	install tcp_china.ko /lib/modules/`uname -r`/kernel/net/ipv4
	insmod /lib/modules/`uname -r`/kernel/net/ipv4/tcp_china.ko
	depmod -a