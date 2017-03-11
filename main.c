// F18A In-System Updater
// TI-99/4A 512k ROM version
// This version uses the PureGPU updater
// code and is intended as a template
// for porting to other systems.
//
// The goal is to keep the host stub as simple
// as possible for easier porting. All you need
// is to init your system, load the GPU code,
// and handle input and file access.
//
// @author Tursi (Mike Brent)
// @date Mar 2017
// @version 1.7.1
// 
// Based on:
// @author Matthew Hagerty
// @author Rasmus (lastname?)
// @date July 2014
// @version 1.6.2
//
// Follow the usual instructions to update the main code CRCs and checksum
// in the original disk-based updater.
//
// Once the main program is updated, can update F18A_PureGPU.a99 (note that
// it will have a different checksum and requires that step repeated).
// Build F18A_PureGPU.a99 and convert the resulting object file into a raw binary.
// Include the binary as header here (doing it this way so that it can be more
// easily included in non-9900-based systems)
//

// helpful defines for bank switching
#include "banking.h"
// brings in GPUDAT - the assembled 9900 code for the GPU
#include "F18A_PureGPU.h"
// brings in the character set
#include "chars.h"

// prototypes
void vdpinit();
unsigned char kscanfast();
void main();
void errret();
void gored();
void dodsr(unsigned int pab);
void dsrread(unsigned int pab);
void do_gpu(unsigned int adr);
void cont_gpu();
void f18adt();
void gmode();
void vsbw(unsigned int VDP, unsigned char val);
void vsmw(unsigned int VDP, unsigned char val, unsigned int cnt);
void vmbw_slow(unsigned int VDP, unsigned char *cpu, unsigned int cnt);
void vmbw(unsigned int VDP, unsigned char *cpu, unsigned int cnt);
void vmbr_slow(unsigned int VDP, unsigned char *cpu, unsigned int cnt);
void vmbr(unsigned int VDP, unsigned char *cpu, unsigned int cnt);
void vwtr(unsigned int VDP);
void loadch();

//////////////////////////////////////
// Initialization functions
//////////////////////////////////////

// sound access
volatile __sfr __at 0xff SOUND;

// Coleco's sound generator comes up making random noise
inline void MUTE_SOUND() { 
	SOUND=0x9f;		// mute chan 1 
	SOUND=0xbf;		// mute chan 2
	SOUND=0xdf;		// mute chan 3
	SOUND=0xff;		// mute noise 
}

// VDP access
// Read Data
volatile __sfr __at 0xbe VDPRD;
// Read Status
volatile __sfr __at 0xbf VDPST;
// Write Address/Register
volatile __sfr __at 0xbf VDPWA;
// Write Data
volatile __sfr __at 0xbe VDPWD;

// helper for VDP delay between accesses
inline void VDP_SAFE_DELAY() {	
__asm
	nop
	nop
	nop
	nop
	nop
__endasm;
}

// just mutes the sound chip - the rest of the program inits the VDP
// this is called from the crt0 startup
void vdpinit() {
	// mute the sound chip
	MUTE_SOUND();
}

/////////////////////////////////////////////////
// Hardware and Config
/////////////////////////////////////////////////

// bank
unsigned int nBank;

// 0xffff is the program page, 0xfffe is the first data page, counting down.
// Pages are 16k in size.
const unsigned int FILE1S = 0xfff1; // First page of file 1 (166k takes 11 pages)
const unsigned int FILE2S = 0xfffc; // First page of file 2 (46k takes 3 pages)
const unsigned int FILE3S = 0xffff; // First page if there was a file 3

// ports for controller access
static volatile __sfr __at 0xfc port0;
static volatile __sfr __at 0xff port1;
static volatile __sfr __at 0x80 port2;
static volatile __sfr __at 0xc0 port3;
#define SELECT 0x2a

// controller keypad map
const unsigned char keys[16] = {
	0xff, '8', '4', '5', 
	0xff, '7', '#', '2',
	0xff, '*', 'Q', '9',		// '0' to Quit
	'3',  'P', '6', 0xff		// '1' to Proceed
};

// VDP Memory Map
const unsigned int NAMETB = 0x0000;  // Name table
const unsigned int COLRTB = 0x0380;  // Color table
const unsigned int PTRNTB = 0x0800;  // Pattern table
const unsigned int SPRATB = 0x0300;  // Sprite attribute table
const unsigned int SPRPTB = 0x0800;  // Sprite pattern table
const unsigned int PABBUF = 0x1000;	 // File data buffer

// GPU Interface (all addresses in VDP)
const unsigned int GPUPRG = 0x2800; // GPU program start
const unsigned int GPUKEY = 0x2802;	// write keypresses here - FF means none
const unsigned int GPURES = 0x2804;	// read GPU result from here - 0 means success
const unsigned char ADRRES = 0;			// read GPU result here - 0 means success
const unsigned char DSRRES = 1;			// GPU DSR request PAB address here - 0 means none
const unsigned char CCKRES = 2;			// GPU calculated checksum
const unsigned char ECKRES = 3;			// GPU expected checksum

// GPU test program (9900 asm, GPUPRG == 0x2800)
const unsigned char GPUDT[] = {
	0x04,0xe0,	// CLR @GPUPRG
	0x28,0x00,
	0x03,0x40		// IDLE
};

// Status data
unsigned char F18A;		// F18A detected
unsigned int  R5[4];		// GPU status reads into four words here
unsigned char PAB[10];	// PAB data is read into five words here

#define F18ERR "F18A NOT DETECTED"
#define CHKERR "CHECKSUM ERROR"

// interface definitions for TI-style DSR simulation
// through a Peripheral Access Block
#define PAB_RECORD_SIZE 128
#define PAB_BYTES_DESIRED 10

#define PAB_OPCODE_OPEN 0
#define PAB_OPCODE_CLOSE 1
#define PAB_OPCODE_READ 2

#define PAB_OPEN_ATTRIBUTES 0x0C

#define PABERR_BAD_ATTRIBUTE	0x40
#define PABERR_READ_PAST_EOF	0xA0
#define PABERR_BAD_FILENAME		0xE0

#define PABIDX_OPCODE	0
#define PABIDX_ERROR	1
#define PABIDX_VDPBUF	2
#define PABIDX_REQLEN	4
#define PABIDX_GOTLEN	5
#define PABIDX_RECORD	6

#define RECORDS_PER_PAGE 126
#define PAGE_BASE 0xC000

/////////////////////////////////////////////////
// Main code
/////////////////////////////////////////////////

// read the left keypad and return the pressed key or 0xff if none
unsigned char kscanfast() {
	unsigned char key;

	port2 = SELECT;		// select keypad
	VDP_SAFE_DELAY();	// convenient delay to settle
	key = port0;			// read data

	// bits: xFxxNNNN (F - active low fire, NNNN - index into above table)
	return keys[key & 0xf];
}

// ** Main entry point **
void main() {
	// on entry, the VDP is set up, the audio is muted, so all we need to do is
	// set a default bank and write our strings. Note that because our NMI is just
	// a ret, once the first VDP interrupt happens it is never cleared and never triggers
	// again. So we're safe from interrupt madness.
	SWITCH_IN_BANK1;	// doesn't matter which one, the main point is just to get nBank initialized

	// note: this is a rather naive port of the TI assembly code. thus structure.

	// Detect F18A
	F18A=0;
	f18adt();
	
	// graphics mode
	gmode();

	// Check for F18A
	if (!F18A) {
		// Diplay error if not present
		vmbw_slow(11*32+7+NAMETB, F18ERR, 17);
		gored();	// never returns
	}
	
	// Copy GPU code to VDP RAM
	vmbw(GPUPRG, GPUDAT, SIZE_OF_GPUDAT);

	// set the initial GPU address
	do_gpu(GPUPRG);

	// At this point, we just slave to the GPU
	// We need to read the keyboard and feed the code to it
	// It only cares about three keys: P (proceed), Q (quit), 0xff (none)
	for (;;) {
		vwtr(0x0f02);		// Set the status port to read SR2

		// loop while the GPU is still running
		do {
			// read the keyboard and forward the result to the GPU
			vsbw(GPUKEY, kscanfast());
		} while (VDPST&0x80);
       
		// the GPU has stopped, restore the status register
		vwtr(0x0f00);		// Set status port to read SR0
       
		// read program status into registers
		vmbr(GPURES, (unsigned char*)R5, 8);
             
		// first check for error
		if (R5[ADRRES]) {
			errret();	// never returns
		}
             
		// no error, check for a DSR request
		if (R5[DSRRES]) {
			// fix endianess of 16-bit value
			int	pab = ((R5[DSRRES]&0xff00)>>8) | ((R5[DSRRES]&0xff)<<8);
			dodsr(pab);
			cont_gpu();
		} else {
			// not error, not DSR, must be success!
		  break;
		}
	}
		
	// then it must have been successful! Set the screen to green
	// and spin forever - user must power off.
	vwtr(0x0702);
	for (;;) {}
}

// GPU returned a problem - go red       
void errret() {
	// original code would reboot on checksum error, so we can
	// check and be a little nicer here
	if (R5[CCKRES] != R5[ECKRES]) {
		// checksum mismatch - everything else the GPU reports
		vmbw(8*32+1, CHKERR, 14);
	}
	
	gored();	// never returns
}

// set the screen to red and lock up
void gored() {
	vwtr(0x0708);
	for (;;) { }
}

// GPU has requested a DSR operation
void dodsr(unsigned int pab) {
	// again, be careful, numbers are big endian
	vmbr(pab, PAB, PAB_BYTES_DESIRED);		// 0=opcode/status,1=buffer,2=record/size,3=rec#,4=off/name
             
	// check close - it ignores attributes
	if (PAB[PABIDX_OPCODE] == PAB_OPCODE_CLOSE) {
		// this is just a close request, ignore it
		return;
	}
	
	// basic sanity
	if (PAB[PABIDX_REQLEN] != PAB_RECORD_SIZE) {
		// bad attribute
		PAB[PABIDX_ERROR]|=PABERR_BAD_ATTRIBUTE;
		vmbw(pab, PAB, PAB_BYTES_DESIRED);
		return;
	}

	if (PAB[PABIDX_OPCODE] == PAB_OPCODE_READ) {
		// it's read
		dsrread(pab);
		vmbw(pab, PAB, PAB_BYTES_DESIRED);
		return;
	}
	
	// if we get here, it's open or nothing, so check
	// both the command and the attributes
	if ((PAB[PABIDX_OPCODE] != PAB_OPCODE_OPEN) || (PAB[PABIDX_ERROR] != PAB_OPEN_ATTRIBUTES)) {
		// bad attribute
		PAB[PABIDX_ERROR]|=PABERR_BAD_ATTRIBUTE;
		vmbw(pab, PAB, PAB_BYTES_DESIRED);
		return;
	}
}	
             
// read a record - it should be a 128 byte block
// for the sake of simplicity, each 16k page has
// 126 128 byte records. (256 bytes are left free
// at the end of each block for banking addresses).
// No record is ever split across pages. 
// PAB is global (and only has 10 bytes)
// pab is already in the correct endianess here
void dsrread(unsigned int pab) {
	// first, figure out which file we're reading and
	// check whether the record is in range. Note that
	// the EOF is only accurate to within 128 records,
	// so probably isn't very helpful here. ;)
  unsigned char x[2];
  unsigned int firstblock, nextfile;
  unsigned int block, rec, buf;
  
  vmbr(pab+19, x, 2);		// reads 'BI' or 'RO' from filename

	if ((x[0] == 'B') && (x[1] == 'I')) {
		// file 1
		firstblock = FILE1S;
		nextfile = FILE2S;
	} else if ((x[0] == 'R') && (x[1] == 'O')) {
		// file 2
		firstblock = FILE2S;
		nextfile = FILE3S;
	} else {
		// unknown filename
		PAB[PABIDX_ERROR] |= PABERR_BAD_FILENAME;
		return;
	}

	// get big endian record number
	rec = (PAB[PABIDX_RECORD]<<8) | PAB[PABIDX_RECORD+1];			// record number
	block = rec / RECORDS_PER_PAGE + firstblock;	// figure out which page address
	
	// we also check for wraparound, since we're so close to it
	// you should check block >= nextfile, but the wraparound
	// check is probably Coleco specific
	if ((block >= nextfile) || (block < FILE1S)) {
		// past end of file (probably by a lot ;) )
		PAB[PABIDX_ERROR] |= PABERR_READ_PAST_EOF;
		return;
	}
	
	// activate the bank
	SWITCH_IN_OLD_BANK(block);
	
	// get the record's offset in memory
	rec = (rec % RECORDS_PER_PAGE) * PAB_RECORD_SIZE + PAGE_BASE;
	
	// get big endian VDP buffer
	buf = (PAB[PABIDX_VDPBUF]<<8) | PAB[PABIDX_VDPBUF+1];
	
	// copy the data to the requested VDP address
	vmbw(buf, (unsigned char*)rec, PAB_RECORD_SIZE);
	
	// copy is complete, update the PAB (not much to do, we don't have errors)
	
	// increment record number (big endian)
	if (++PAB[PABIDX_RECORD+1] == 0) {
		++PAB[PABIDX_RECORD];
	}
	
	// set read record length
	PAB[PABIDX_GOTLEN]=PAB_RECORD_SIZE;
	
	return;
}

// Call a GPU routine - do not wait!
void do_gpu(unsigned int adr) {
	vwtr(0x3600+(adr>>8));		// VR36 = MSB of GPU address
	vwtr(0x3700+(adr&0xff));	// VR37 = LSB of GPU address (starts GPU)
}

// Restart the GPU without setting the address
void cont_gpu() {
	vwtr(0x3801);							// GPU GO bit to 1
}

// Detect F18A
void f18adt() {
	unsigned char x[2];
	
	// F18A Unlock
	vwtr(0x391c);		// VR1/57, value 00011100
	vwtr(0x391c);		// Write twice, unlock
	vwtr(0x01E0);		// VR1, value 11100000, a real sane setting

	// Copy GPU test code to VRAM
	vmbw(GPUPRG, GPUDT, 6);

	// start it running
	do_gpu(GPUPRG);

	// Compare the result in GPUPRG
	vmbr_slow(GPUPRG, x, 2);
	if ((x[0] != 0) || (x[1] != 0)) {
		// code didn't run, no F18A
		F18A = 0;
		return;
	}
	
	// code ran, so we have one
	F18A = 1;
}

// Setup graphics mode (this is a bit redundant... GPU does it too)
void gmode() {
	vwtr(0x0000);	// Reg 0: Graphics mode I, external video off
	vwtr(0x01c2);	// Reg 1: 16K, display on, no interrupt, size = 1, mag = 0.
	vwtr(0x0200);	// Reg 2: Name table NAMETB = >1800 (>06 * >400), >300 bytes
	vwtr(0x030e);	// Reg 3: Color Table COLRTB = >0380 (>0E * >40), >20 bytes
	vwtr(0x0401);	// Reg 4: Pattern Table PTRNTB = >0800 (>01 * >800), >800 bytes
	vwtr(0x0506);	// Reg 5: Sprite Attribute Table SPRATB = >0300 (>06 * >80), >80 bytes
	vwtr(0x0601);	// Reg 6: Sprite Pattern Table SPRPTB = >0800 (>01 * >800), >800 bytes
	vwtr(0x07f4);	// Reg 7: Text-mode color and backdrop color Blue backdrop

	// Disable sprites
	vsbw(SPRATB, 0xd);

	// Initialize color table
	vsmw(COLRTB, 0xf0, 32);	// white on transparent

	// Clear name table
	vsmw(NAMETB, 0x20, 0x300);	// Space

	// Load character set (optional, can remove if you need space)
	loadch();
}

// The need for slow VDP functions depends on your system, but
// most systems need them for the case where someone tries to
// run this code on a 99x8A instead of an F18A.

// VDP Single Byte Write
// this function is slow and safe
void vsbw(unsigned int VDP, unsigned char val) {
	VDPWA = VDP&0xff;				// Send low byte of VDP RAM write address
	VDPWA = (VDP>>8)|0x40;	// Set read/write bits 14 and 15 to write (01)
	
	VDP_SAFE_DELAY();				// delay for safety - might not be F18A
	VDPWD = val;						// Write byte to VDP RAM
	VDP_SAFE_DELAY();				// delay for safety - might not be F18A
}

// VDP Single Byte Multiple Write
// this function is slow and safe
void vsmw(unsigned int VDP, unsigned char val, unsigned int cnt) {
	VDPWA = VDP&0xff;				// Send low byte of VDP RAM write address
	VDPWA = (VDP>>8)|0x40;	// Set read/write bits 14 and 15 to write (01)
	
	VDP_SAFE_DELAY();				// delay for safety - might not be F18A

	while (cnt--) {
		VDPWD = val;
		VDP_SAFE_DELAY();
	}
}

// VDP Multiple Byte Write
// this function is slow and safe
void vmbw_slow(unsigned int VDP, unsigned char *cpu, unsigned int cnt) {
	VDPWA = VDP&0xff;				// Send low byte of VDP RAM write address
	VDPWA = (VDP>>8)|0x40;	// Set read/write bits 14 and 15 to write (01)
	
	VDP_SAFE_DELAY();				// delay for safety - might not be F18A

	while (cnt--) {
		VDPWD = *(cpu++);
		VDP_SAFE_DELAY();
	}
}

// VDP Multiple Byte Write
// this function is fast and F18A only
void vmbw(unsigned int VDP, unsigned char *cpu, unsigned int cnt) {
	VDPWA = VDP&0xff;				// Send low byte of VDP RAM write address
	VDPWA = (VDP>>8)|0x40;	// Set read/write bits 14 and 15 to write (01)

	while (cnt--) {
		VDPWD = *(cpu++);
	}
}

// VDP Multiple Byte Read
// this function is slow and safe
void vmbr_slow(unsigned int VDP, unsigned char *cpu, unsigned int cnt) {
	VDPWA = VDP&0xff;				// Send low byte of VDP RAM write address
	VDPWA = (VDP>>8);				// Send high byte of VDP RAM write address
	
	VDP_SAFE_DELAY();				// delay for safety - might not be F18A

	while (cnt--) {
		*(cpu++) = VDPRD;
		VDP_SAFE_DELAY();
	}
}

// VDP Multiple Byte Read
// this function is fast and F18A only
void vmbr(unsigned int VDP, unsigned char *cpu, unsigned int cnt) {
	VDPWA = VDP&0xff;				// Send low byte of VDP RAM write address
	VDPWA = (VDP>>8);				// Send high byte of VDP RAM write address

	while (cnt--) {
		*(cpu++) = VDPRD;
	}
}

// VDP Write To Register
void vwtr(unsigned int VDP) {
	VDPWA = VDP&0xff;				// Send low byte of VDP RAM write address
	VDPWA = (VDP>>8)|0x80;	// Set read/write bits 14 and 15 to register set (10)
}

// load the character set to PDT.
void loadch() {
	// slow cause we might not be on F18A
	vmbw_slow(PTRNTB + (CHARS_FIRST_CHAR*8), CHARS, SIZE_OF_CHARS);	
}

