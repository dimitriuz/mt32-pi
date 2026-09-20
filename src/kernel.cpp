//
// kernel.cpp
//
// mt32-pi - A baremetal MIDI synthesizer for Raspberry Pi
// Copyright (C) 2020-2023 Dale Whinham <daleyo@gmail.com>
//
// This file is part of mt32-pi.
//
// mt32-pi is free software: you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version.
//
// mt32-pi is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
// details.
//
// You should have received a copy of the GNU General Public License along with
// mt32-pi. If not, see <http://www.gnu.org/licenses/>.
//

#include <circle/memio.h>
#include <circle/string.h>
#include <fatfs/ff.h>

#include "config.h"
#include "kernel.h"

#ifndef MT32_PI_VERSION
#define MT32_PI_VERSION "<unknown>"
#endif

CKernel::CKernel(void)
	: CStdlibApp("mt32-pi"),

	  m_Serial(&mInterrupt, true),
#ifdef HDMI_CONSOLE
	  m_Screen(mOptions.GetWidth(), mOptions.GetHeight()),
#endif

	  m_Timer(&mInterrupt),
	  m_Logger(mOptions.GetLogLevel(), &m_Timer),
	  m_USBHCI(&mInterrupt, &m_Timer, true),
	  m_EMMC(&mInterrupt, &m_Timer, &mActLED),
	  m_SDFileSystem{},

	  m_I2CMaster(1, true),
	  m_GPIOManager(&mInterrupt),

	  m_MT32Pi(&m_I2CMaster, &m_SPIMaster, &mInterrupt, &m_GPIOManager, &m_Serial, &m_USBHCI)
{
}

bool CKernel::Initialize(void)
{
	if (!CStdlibApp::Initialize())
		return false;

#ifdef HDMI_CONSOLE
	if (!m_Screen.Initialize())
		return false;
#endif

	const char* pLogDeviceName = mOptions.GetLogDevice();
	const bool bSerialMIDIAvailable = strcmp(pLogDeviceName, "ttyS1") != 0;

	// Init serial port early if used for logging
	if (!bSerialMIDIAvailable && !m_Serial.Initialize(115200))
		return false;

	CDevice* pLogTarget = mDeviceNameService.GetDevice(pLogDeviceName, false);

	if (!pLogTarget)
		pLogTarget = &mNullDevice;

	if (!m_Logger.Initialize(pLogTarget))
		return false;

	if (!m_Timer.Initialize())
		return false;

	if (!m_EMMC.Initialize())
		return false;

	if (f_mount(&m_SDFileSystem, "SD:", 1) != FR_OK)
	{
		m_Logger.Write(GetKernelName(), LogError, "Failed to mount SD card");
		return false;
	}

	// Load configuration file
	if (!m_Config.Initialize("mt32-pi.cfg"))
		m_Logger.Write(GetKernelName(), LogWarning, "Unable to find or parse config file; using defaults");

	// Init serial port for MIDI with preferred baud rate if not used for logging
	if (bSerialMIDIAvailable && !m_Serial.Initialize(m_Config.MIDIGPIOBaudRate))
		return false;

	// Init I2C. On the Raspberry Pi 5 this is a DesignWare controller in the
	// RP1, not the BCM2835/BCM2711 BSC, and Initialize() is not optional there:
	// it checks the component type, disables the adapter (DW_IC_CON and
	// DW_IC_TAR are only writable while it is disabled), sets the Tx/Rx FIFO
	// thresholds that Transfer() polls, and masks the interrupts. Skipping it
	// leaves every transfer failing, which takes the LCD, MiSTer control and
	// I2S DAC configuration with it.
	m_bI2CInitOK = m_I2CMaster.Initialize();
	if (!m_bI2CInitOK)
		m_Logger.Write(GetKernelName(), LogWarning, "I2C init failed; LCD, MiSTer control and I2S DAC setup will not work");

	m_I2CMaster.SetClock(m_Config.SystemI2CBaudRate);

	// TEMP: probe before any other subsystem has come up, to tell an I2C bus
	// that is broken from the start apart from one broken by later init.
	{
		extern u32 g_nMT32PiI2CAbortSource;
		g_nMT32PiI2CAbortSource = 0;
		u8 ucZero = 0;
		m_nEarlyProbeResult = m_I2CMaster.Write(0x3c, &ucZero, 1);
		m_nEarlyProbeAbort  = g_nMT32PiI2CAbortSource;
	}

	// Init SPI
	if (!m_SPIMaster.Initialize())
		return false;

	// Init GPIO manager
	if (!m_GPIOManager.Initialize())
		return false;

	// Init custom memory allocator
	if (!m_Allocator.Initialize())
		return false;

	if (!m_MT32Pi.Initialize(bSerialMIDIAvailable))
		return false;

	return true;
}

CStdlibApp::TShutdownMode CKernel::Run(void)
{
	m_Logger.Write(GetKernelName(), LogNotice, "mt32-pi " MT32_PI_VERSION);
	m_Logger.Write(GetKernelName(), LogNotice, "Compile time: " __DATE__ " " __TIME__);

	LogI2CScan();

	m_MT32Pi.Run(0);

	return ShutdownReboot;
}

// Temporary bring-up diagnostic for the Raspberry Pi 5/500. Dumps the RP1 pin
// muxing and DesignWare I2C controller state, probes the bus, and reports the
// TX_ABRT_SOURCE that Circle otherwise collapses into a generic NACK.
// Written to SD:/mt32-pi-diag.txt so it can be retrieved over FTP.

extern u32 g_nMT32PiI2CAbortSource;		// TEMP, set by Circle's RP1 I2C driver

#define RP1_GPIO_STATUS(pin)	(0x1F000D0000UL + (pin) * 8)
#define RP1_GPIO_CTRL(pin)	(0x1F000D0000UL + (pin) * 8 + 4)
#define RP1_PADS_CTRL(pin)	(0x1F000F0000UL + 4 + (pin) * 4)

#define I2C1_BASE		0x1F00074000UL
#define DW_CON			0x00
#define DW_TAR			0x04
#define DW_SS_SCL_HCNT		0x14
#define DW_SS_SCL_LCNT		0x18
#define DW_FS_SCL_HCNT		0x1c
#define DW_FS_SCL_LCNT		0x20
#define DW_RAW_INTR_STAT	0x34
#define DW_ENABLE		0x6c
#define DW_TX_ABRT_SOURCE	0x80
#define DW_ENABLE_STATUS	0x9c
#define DW_COMP_TYPE		0xfc

void CKernel::LogI2CScan(void)
{
	CString Report;
	CString Line;

	#define EMIT(...)                                                   \
		do {                                                        \
			Line.Format(__VA_ARGS__);                           \
			m_Logger.Write(GetKernelName(), LogNotice,          \
				       static_cast<const char*>(Line));     \
			Report.Append(static_cast<const char*>(Line));      \
			Report.Append("\r\n");                              \
		} while (0)

	EMIT("I2C diag: CI2CMaster::Initialize() returned %s", m_bI2CInitOK ? "true" : "false");

	// RP1 pin muxing. FUNCSEL 3 is I2C1 on GPIO 2/3; PADS bit 6 is input
	// enable, bit 7 is output disable. Without IE the controller cannot see
	// a slave pulling SDA low to acknowledge.
	for (unsigned nPin = 2; nPin <= 3; ++nPin)
	{
		const u32 nCtrl = read32(RP1_GPIO_CTRL(nPin));
		const u32 nPads = read32(RP1_PADS_CTRL(nPin));
		EMIT("GPIO%u: CTRL=0x%08x (FUNCSEL=%u) PADS=0x%08x (IE=%u OD=%u)",
		     nPin, nCtrl, nCtrl & 0x1F, nPads,
		     (nPads >> 6) & 1, (nPads >> 7) & 1);
	}

	// Raw pad levels. I2C idles with both lines pulled high; INFROMPAD reading
	// 0 means something is holding that line low, which is what makes the
	// controller think it lost arbitration the moment it drives SDA high.
	for (unsigned nPin = 2; nPin <= 3; ++nPin)
	{
		const u32 nStatus = read32(RP1_GPIO_STATUS(nPin));
		EMIT("GPIO%u STATUS=0x%08x: INFROMPAD=%u INFILTERED=%u INTOPERI=%u OUTTOPAD=%u OETOPAD=%u  (%s)",
		     nPin, nStatus,
		     (nStatus >> 17) & 1, (nStatus >> 18) & 1, (nStatus >> 19) & 1,
		     (nStatus >> 9) & 1, (nStatus >> 13) & 1,
		     ((nStatus >> 17) & 1) ? "line HIGH - idle, as expected" : "line LOW - STUCK");
	}

	const u32 nCompType = read32(I2C1_BASE + DW_COMP_TYPE);
	EMIT("I2C1 COMP_TYPE=0x%08x (expected 0x44570140) %s",
	     nCompType, nCompType == 0x44570140 ? "OK" : "MISMATCH");
	EMIT("I2C1 CON=0x%08x TAR=0x%08x ENABLE=0x%08x ENABLE_STATUS=0x%08x",
	     read32(I2C1_BASE + DW_CON), read32(I2C1_BASE + DW_TAR),
	     read32(I2C1_BASE + DW_ENABLE), read32(I2C1_BASE + DW_ENABLE_STATUS));
	EMIT("I2C1 SS_HCNT=%u SS_LCNT=%u FS_HCNT=%u FS_LCNT=%u RAW_INTR=0x%08x",
	     read32(I2C1_BASE + DW_SS_SCL_HCNT), read32(I2C1_BASE + DW_SS_SCL_LCNT),
	     read32(I2C1_BASE + DW_FS_SCL_HCNT), read32(I2C1_BASE + DW_FS_SCL_LCNT),
	     read32(I2C1_BASE + DW_RAW_INTR_STAT));

	EMIT("early probe 0x3c (before other init): result=%d ABRT_SOURCE=0x%08x",
	     m_nEarlyProbeResult, m_nEarlyProbeAbort);

	// The SSD1306 is write-only, so probe it the way it is actually used.
	// 0x45 is the MiSTer control interface.
	static const struct { u8 nAddress; const char* pName; } Probes[] =
	{
		{0x3c, "LCD"},
		{0x45, "MiSTer"},
	};

	for (const auto& Probe : Probes)
	{
		g_nMT32PiI2CAbortSource = 0;

		u8 ucZero = 0;
		const int nResult = m_I2CMaster.Write(Probe.nAddress, &ucZero, 1);
		const u32 nAbort = g_nMT32PiI2CAbortSource;

		EMIT("probe 0x%02x (%s): result=%d ABRT_SOURCE=0x%08x%s%s%s%s",
		     Probe.nAddress, Probe.pName, nResult, nAbort,
		     nAbort & (1u << 0)  ? " 7B_ADDR_NOACK" : "",
		     nAbort & (1u << 3)  ? " TXDATA_NOACK"  : "",
		     nAbort & (1u << 11) ? " MASTER_DIS"    : "",
		     nAbort & (1u << 12) ? " ARB_LOST"      : "");
	}

	// Raspberry Pi OS talks to this board fine at its 100 kHz default, while
	// mt32-pi runs the bus at 400 kHz. If SDA cannot rise fast enough the
	// master reads back low while driving high, which is exactly ARB_LOST.
	for (unsigned nClock = 100000; nClock <= 200000; nClock += 100000)
	{
		m_I2CMaster.SetClock(nClock);

		g_nMT32PiI2CAbortSource = 0;
		u8 ucZero = 0;
		const int nResult = m_I2CMaster.Write(0x3c, &ucZero, 1);
		const u32 nAbort = g_nMT32PiI2CAbortSource;

		EMIT("probe 0x3c @ %u Hz: result=%d ABRT_SOURCE=0x%08x%s%s",
		     nClock, nResult, nAbort,
		     nAbort & (1u << 0)  ? " 7B_ADDR_NOACK" : "",
		     nAbort & (1u << 12) ? " ARB_LOST"      : "");
	}

	#undef EMIT

	FIL File;
	if (f_open(&File, "SD:/mt32-pi-diag.txt", FA_WRITE | FA_CREATE_ALWAYS) == FR_OK)
	{
		UINT nWritten;
		f_write(&File, static_cast<const char*>(Report), Report.GetLength(), &nWritten);
		f_close(&File);
	}
	else
		m_Logger.Write(GetKernelName(), LogWarning, "Could not write SD:/mt32-pi-diag.txt");
}
