# F18A updater - 256k
# ROM layout:
#
# 16k blank
# 176k of bitstream
# 48k of data block
# 16k of program


CC = "c:/program files (x86)/sdcc/bin/sdcc"
CFLAGS = -mz80 -c "-I../include" --std-sdcc99 --vc -DENABLEFX --opt-code-speed
AS = "c:/program files (x86)/sdcc/bin/sdasz80"
AR = "c:/program files (x86)/sdcc/bin/sdar"
AFLAGS = -plosgff

.PHONY: all clean

# crt0 must be first! (note this is also why the output is named crt0.ihx)
# list all your object files in this variable
# note that the order of the banks is not important in this line
objs = crt0.rel main.rel

# this builds the final ROM
# warning: main.c is expected to build to less than 8192. It MUST build
# to less than 16128 bytes (and fix setbinsize below)
all: buildcoleco
	makemegacart.exe -map crt0.s crt0.ihx pureldr.bin
	setbinsize pureldr.bin 8192
	packdatacart F18APureGPU_Coleco.rom pureldr.bin f18a_250k_V18.bit f18a_rom_data.bin

# this rule links the files into the ihx
buildcoleco: $(objs) banking.h chars.h F18A_PureGPU.h
	$(CC) -mz80 --no-std-crt0 --code-loc 0x8100 --data-loc 0x7000 $(objs)

# clean up the build
clean:
	-rm *.rel *.map *.lst *.sym *.asm *.ihx *.rom

# build the crt0 startup code
crt0.rel: crt0.s
	$(AS) $(AFLAGS) crt0.rel crt0.s

# build the source files
main.rel: main.c
	$(CC) $(CFLAGS) -o main.rel main.c --codeseg main --constseg main


