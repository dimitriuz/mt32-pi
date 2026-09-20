//
// debugserver.cpp
//
// mt32-pi - A baremetal MIDI synthesizer for Raspberry Pi
//
// TEMPORARY bring-up tool for the Raspberry Pi 5/500 port. See debugserver.h.
//

#include <circle/gpiopin.h>
#include <circle/logger.h>
#include <circle/memio.h>
#include <circle/net/in.h>
#include <circle/net/netsubsystem.h>
#include <circle/sched/scheduler.h>
#include <circle/timer.h>
#include <circle/util.h>

#include <circle/serial.h>

#include "mt32pi.h"
#include "net/debugserver.h"

LOGMODULE("debugserver");

constexpr u16 DebugPort = 3333;

// RP1 register map, bank 0
#define RP1_GPIO_STATUS(pin)	(0x1F000D0000UL + (pin) * 8)
#define RP1_GPIO_CTRL(pin)	(0x1F000D0000UL + (pin) * 8 + 4)
#define RP1_PADS_CTRL(pin)	(0x1F000F0000UL + 4 + (pin) * 4)

#define I2C1_BASE		0x1F00074000UL

constexpr u8 GPIOSDA = 2;
constexpr u8 GPIOSCL = 3;

// Set by Circle's RP1 I2C driver, which otherwise discards the abort reason.
extern u32 g_nMT32PiI2CAbortSource;

static bool ParseU64(const char* pString, u64& rValue)
{
	if (!pString || !*pString)
		return false;

	// Accept both 0x-prefixed and bare hex.
	if (pString[0] == '0' && (pString[1] == 'x' || pString[1] == 'X'))
		pString += 2;

	u64 nValue = 0;
	for (; *pString; ++pString)
	{
		u64 nDigit;
		if (*pString >= '0' && *pString <= '9')
			nDigit = *pString - '0';
		else if (*pString >= 'a' && *pString <= 'f')
			nDigit = *pString - 'a' + 10;
		else if (*pString >= 'A' && *pString <= 'F')
			nDigit = *pString - 'A' + 10;
		else
			return false;

		nValue = nValue * 16 + nDigit;
	}

	rValue = nValue;
	return true;
}

static bool ParseDecimal(const char* pString, u64& rValue)
{
	if (!pString || !*pString)
		return false;

	u64 nValue = 0;
	for (; *pString; ++pString)
	{
		if (*pString < '0' || *pString > '9')
			return false;
		nValue = nValue * 10 + (*pString - '0');
	}

	rValue = nValue;
	return true;
}

CDebugServer::CDebugServer(CI2CMaster* pI2CMaster, CMT32Pi* pMT32Pi)
	: CTask(TASK_STACK_SIZE, true),
	  m_pSocket(nullptr),
	  m_pI2CMaster(pI2CMaster),
	  m_pMT32Pi(pMT32Pi),
	  m_Buffer{0}
{
}

CDebugServer::~CDebugServer()
{
	if (m_pSocket)
		delete m_pSocket;
}

bool CDebugServer::Initialize()
{
	assert(m_pSocket == nullptr);

	CNetSubSystem* const pNet = CNetSubSystem::Get();

	if ((m_pSocket = new CSocket(pNet, IPPROTO_UDP)) == nullptr)
		return false;

	if (m_pSocket->Bind(DebugPort) != 0)
	{
		LOGERR("Couldn't bind to port %d", DebugPort);
		return false;
	}

	LOGNOTE("Debug server listening on UDP port %d", DebugPort);

	Start();

	return true;
}

void CDebugServer::Run()
{
	assert(m_pSocket != nullptr);

	CScheduler* const pScheduler = CScheduler::Get();

	while (true)
	{
		CIPAddress ForeignIP;
		u16 nForeignPort;

		const int nResult = m_pSocket->ReceiveFrom(m_Buffer, sizeof(m_Buffer) - 1, 0,
							   &ForeignIP, &nForeignPort);

		if (nResult > 0)
		{
			m_Buffer[nResult] = '\0';

			// Strip trailing newline/CR so `echo cmd | nc -u` works.
			for (int i = nResult - 1; i >= 0 && (m_Buffer[i] == '\n' || m_Buffer[i] == '\r'); --i)
				m_Buffer[i] = '\0';

			CString Response;
			Execute(m_Buffer, Response);

			m_pSocket->SendTo(static_cast<const char*>(Response), Response.GetLength(), 0,
					  ForeignIP, nForeignPort);
		}

		pScheduler->Yield();
	}
}

void CDebugServer::Execute(char* pCommand, CString& rResponse)
{
	// Split into at most 3 whitespace-separated tokens.
	char* pArgs[3] = {nullptr, nullptr, nullptr};
	unsigned nArgs = 0;

	for (char* p = pCommand; *p && nArgs < 3; )
	{
		while (*p == ' ' || *p == '\t')
			++p;
		if (!*p)
			break;

		pArgs[nArgs++] = p;

		while (*p && *p != ' ' && *p != '\t')
			++p;
		if (*p)
			*p++ = '\0';
	}

	if (nArgs == 0)
		return;

	const char* pCmd = pArgs[0];
	u64 nA = 0, nB = 0;

	if (strcmp(pCmd, "help") == 0)
	{
		rResponse.Append("rd <addr>             read32\r\n");
		rResponse.Append("wr <addr> <val>       write32\r\n");
		rResponse.Append("gpio <pin>            dump CTRL/PADS/STATUS\r\n");
		rResponse.Append("i2cinit               CI2CMaster::Initialize()\r\n");
		rResponse.Append("i2cclock <hz>         SetClock (decimal)\r\n");
		rResponse.Append("i2cprobe <addr>       write 1 byte, report abort source\r\n");
		rResponse.Append("i2cscan               probe 0x08-0x77\r\n");
		rResponse.Append("i2crecover            bit-bang 9 SCL pulses + STOP\r\n");
		rResponse.Append("i2cdump               dump I2C1 registers\r\n");
		rResponse.Append("midi <hexbytes>       inject MIDI straight into the parser\r\n");
		rResponse.Append("midistat              UDP MIDI receiver state + packet count\r\n");
		rResponse.Append("bbprobe <addr>        software bit-banged probe\r\n");
		rResponse.Append("drive <pin> <0-3>     pad drive strength (Linux uses 12mA=3)\r\n");
		rResponse.Append("i2cnew <dev> <cfg>    rebuild master, standard mode\r\n");
	}
	else if (strcmp(pCmd, "rd") == 0 && nArgs >= 2 && ParseU64(pArgs[1], nA))
	{
		CString Line;
		Line.Format("[0x%lx] = 0x%08x\r\n", static_cast<unsigned long>(nA), read32(nA));
		rResponse.Append(static_cast<const char*>(Line));
	}
	else if (strcmp(pCmd, "wr") == 0 && nArgs >= 3 && ParseU64(pArgs[1], nA) && ParseU64(pArgs[2], nB))
	{
		write32(nA, static_cast<u32>(nB));
		CString Line;
		Line.Format("[0x%lx] <- 0x%08x (reads 0x%08x)\r\n",
			    static_cast<unsigned long>(nA), static_cast<unsigned>(nB), read32(nA));
		rResponse.Append(static_cast<const char*>(Line));
	}
	else if (strcmp(pCmd, "gpio") == 0 && nArgs >= 2 && ParseDecimal(pArgs[1], nA))
		CmdGPIO(static_cast<unsigned>(nA), rResponse);
	else if (strcmp(pCmd, "i2cinit") == 0)
	{
		const bool bOK = m_pI2CMaster->Initialize();
		CString Line;
		Line.Format("Initialize() = %s\r\n", bOK ? "true" : "false");
		rResponse.Append(static_cast<const char*>(Line));
	}
	else if (strcmp(pCmd, "i2cclock") == 0 && nArgs >= 2 && ParseDecimal(pArgs[1], nA))
	{
		m_pI2CMaster->SetClock(static_cast<unsigned>(nA));
		CString Line;
		Line.Format("SetClock(%u)\r\n", static_cast<unsigned>(nA));
		rResponse.Append(static_cast<const char*>(Line));
	}
	else if (strcmp(pCmd, "i2cprobe") == 0 && nArgs >= 2 && ParseU64(pArgs[1], nA))
		CmdProbe(static_cast<u8>(nA), rResponse);
	else if (strcmp(pCmd, "i2cscan") == 0)
		CmdScan(rResponse);
	else if (strcmp(pCmd, "i2crecover") == 0)
		CmdRecover(rResponse);
	else if (strcmp(pCmd, "i2cdump") == 0)
		CmdDump(rResponse);
	else if (strcmp(pCmd, "midi") == 0 && nArgs >= 2)
		CmdMIDI(pArgs[1], rResponse);
	else if (strcmp(pCmd, "midistat") == 0)
		CmdMIDIStat(rResponse);
	else if (strcmp(pCmd, "bbprobe") == 0 && nArgs >= 2 && ParseU64(pArgs[1], nA))
		CmdBitBangProbe(static_cast<u8>(nA), rResponse);
	else if (strcmp(pCmd, "drive") == 0 && nArgs >= 3 && ParseDecimal(pArgs[1], nA) && ParseDecimal(pArgs[2], nB))
	{
		const uintptr nReg = RP1_PADS_CTRL(nA);
		u32 nPads = read32(nReg);
		nPads &= ~(3u << 4);
		nPads |= (static_cast<u32>(nB) & 3) << 4;
		write32(nReg, nPads);
		CString Line;
		Line.Format("GPIO%u PADS <- 0x%08x (DRIVE=%u)\r\n",
			    static_cast<unsigned>(nA), read32(nReg), static_cast<unsigned>(nB) & 3);
		rResponse.Append(static_cast<const char*>(Line));
	}
	else if (strcmp(pCmd, "i2cnew") == 0 && nArgs >= 3 && ParseDecimal(pArgs[1], nA) && ParseDecimal(pArgs[2], nB))
	{
		// Rebuild the master the way Circle's own 32-i2cshell does:
		// CI2CMaster(device, bFastMode, config). Lets the reference
		// configuration be tried without a rebuild.
		m_pI2CMaster = new CI2CMaster(static_cast<unsigned>(nA), FALSE, static_cast<unsigned>(nB));
		const bool bOK = m_pI2CMaster->Initialize();
		CString Line;
		Line.Format("new CI2CMaster(device=%u, FALSE, config=%u) Initialize()=%s\r\n",
			    static_cast<unsigned>(nA), static_cast<unsigned>(nB), bOK ? "true" : "false");
		rResponse.Append(static_cast<const char*>(Line));
	}
	else
		rResponse.Append("error: unknown or malformed command; try 'help'\r\n");
}

void CDebugServer::CmdGPIO(unsigned nPin, CString& rResponse)
{
	if (nPin > 27)
	{
		rResponse.Append("error: pin out of range\r\n");
		return;
	}

	const u32 nCtrl = read32(RP1_GPIO_CTRL(nPin));
	const u32 nPads = read32(RP1_PADS_CTRL(nPin));
	const u32 nStatus = read32(RP1_GPIO_STATUS(nPin));

	CString Line;
	Line.Format("GPIO%u CTRL=0x%08x FUNCSEL=%u\r\n"
		    "      PADS=0x%08x IE=%u OD=%u PUE=%u PDE=%u DRIVE=%u SCHMITT=%u\r\n"
		    "      STATUS=0x%08x INFROMPAD=%u INFILTERED=%u OUTTOPAD=%u OETOPAD=%u\r\n",
		    nPin, nCtrl, nCtrl & 0x1F,
		    nPads, (nPads >> 6) & 1, (nPads >> 7) & 1, (nPads >> 3) & 1,
		    (nPads >> 2) & 1, (nPads >> 4) & 3, (nPads >> 1) & 1,
		    nStatus, (nStatus >> 17) & 1, (nStatus >> 18) & 1,
		    (nStatus >> 9) & 1, (nStatus >> 13) & 1);
	rResponse.Append(static_cast<const char*>(Line));
}

void CDebugServer::CmdProbe(u8 nAddress, CString& rResponse)
{
	g_nMT32PiI2CAbortSource = 0;

	u8 ucZero = 0;
	const int nResult = m_pI2CMaster->Write(nAddress, &ucZero, 1);
	const u32 nAbort = g_nMT32PiI2CAbortSource;

	CString Line;
	Line.Format("probe 0x%02x: result=%d ABRT_SOURCE=0x%08x%s%s%s%s\r\n",
		    nAddress, nResult, nAbort,
		    nAbort & (1u << 0)  ? " 7B_ADDR_NOACK" : "",
		    nAbort & (1u << 3)  ? " TXDATA_NOACK"  : "",
		    nAbort & (1u << 11) ? " MASTER_DIS"    : "",
		    nAbort & (1u << 12) ? " ARB_LOST"      : "");
	rResponse.Append(static_cast<const char*>(Line));
}

void CDebugServer::CmdScan(CString& rResponse)
{
	CString Found;
	unsigned nFound = 0;

	for (u8 nAddress = 0x08; nAddress <= 0x77; ++nAddress)
	{
		u8 ucZero = 0;
		if (m_pI2CMaster->Write(nAddress, &ucZero, 1) >= 0)
		{
			CString Entry;
			Entry.Format(" 0x%02x", nAddress);
			Found.Append(static_cast<const char*>(Entry));
			++nFound;
		}
	}

	CString Line;
	if (nFound)
		Line.Format("scan: %u device(s):%s\r\n", nFound, static_cast<const char*>(Found));
	else
		Line.Format("scan: no devices responded\r\n");
	rResponse.Append(static_cast<const char*>(Line));
}

void CDebugServer::CmdRecover(CString& rResponse)
{
	// Standard I2C bus recovery: a slave left mid-byte can hold SDA low
	// indefinitely. Clocking SCL until it releases, then issuing a STOP,
	// returns the bus to idle. Circle's RP1 driver has this as a TODO.
	{
		CGPIOPin SDA(GPIOSDA, TGPIOMode::GPIOModeInput);
		CGPIOPin SCL(GPIOSCL, TGPIOMode::GPIOModeOutput);

		unsigned nPulses = 0;
		for (; nPulses < 9; ++nPulses)
		{
			if (SDA.Read() != 0)
				break;

			SCL.Write(LOW);
			CTimer::SimpleusDelay(5);
			SCL.Write(HIGH);
			CTimer::SimpleusDelay(5);
		}

		// STOP condition: SDA low -> high while SCL is high.
		SDA.SetMode(TGPIOMode::GPIOModeOutput);
		SDA.Write(LOW);
		CTimer::SimpleusDelay(5);
		SCL.Write(HIGH);
		CTimer::SimpleusDelay(5);
		SDA.Write(HIGH);
		CTimer::SimpleusDelay(5);

		SDA.SetMode(TGPIOMode::GPIOModeInput);
		const unsigned nSDALevel = SDA.Read();

		CString Line;
		Line.Format("recover: %u clock pulses, SDA now %s\r\n",
			    nPulses, nSDALevel ? "HIGH" : "still LOW");
		rResponse.Append(static_cast<const char*>(Line));
	}

	// Hand the pins back to the I2C peripheral and reconfigure it.
	CGPIOPin SDA(GPIOSDA, TGPIOMode::GPIOModeAlternateFunction3);
	SDA.SetPullMode(GPIOPullModeUp);
	CGPIOPin SCL(GPIOSCL, TGPIOMode::GPIOModeAlternateFunction3);
	SCL.SetPullMode(GPIOPullModeUp);

	const bool bOK = m_pI2CMaster->Initialize();

	CString Line;
	Line.Format("recover: pins returned to I2C, Initialize() = %s\r\n", bOK ? "true" : "false");
	rResponse.Append(static_cast<const char*>(Line));
}

void CDebugServer::CmdDump(CString& rResponse)
{
	static const struct { u32 nOffset; const char* pName; } Regs[] =
	{
		{0x00, "CON"},        {0x04, "TAR"},        {0x14, "SS_HCNT"},
		{0x18, "SS_LCNT"},    {0x1c, "FS_HCNT"},    {0x20, "FS_LCNT"},
		{0x2c, "INTR_STAT"},  {0x30, "INTR_MASK"},  {0x34, "RAW_INTR"},
		{0x38, "RX_TL"},      {0x3c, "TX_TL"},      {0x6c, "ENABLE"},
		{0x70, "STATUS"},     {0x74, "TXFLR"},      {0x78, "RXFLR"},
		{0x7c, "SDA_HOLD"},   {0x80, "ABRT_SOURCE"},{0x9c, "EN_STATUS"},
		{0xa0, "FS_SPKLEN"},  {0xf4, "COMP_PARAM1"},{0xfc, "COMP_TYPE"},
	};

	for (const auto& Reg : Regs)
	{
		CString Line;
		Line.Format("%-12s [+0x%02x] = 0x%08x\r\n",
			    Reg.pName, Reg.nOffset, read32(I2C1_BASE + Reg.nOffset));
		rResponse.Append(static_cast<const char*>(Line));
	}
}

// Software bit-banged I2C. Open-drain is emulated the usual way: drive a line
// low by making it an output, release it by making it an input and letting the
// pull-up do the work. This bypasses the RP1 I2C controller entirely, so an ACK
// here proves the wiring and Circle's GPIO layer are both fine.
#define BB_DELAY()	CTimer::SimpleusDelay(5)

static void BBRelease(CGPIOPin& rPin)
{
	rPin.SetMode(TGPIOMode::GPIOModeInputPullUp);
}

static void BBDriveLow(CGPIOPin& rPin)
{
	rPin.SetMode(TGPIOMode::GPIOModeOutput);
	rPin.Write(LOW);
}

void CDebugServer::CmdBitBangProbe(u8 nAddress, CString& rResponse)
{
	CGPIOPin SDA(GPIOSDA, TGPIOMode::GPIOModeInputPullUp);
	CGPIOPin SCL(GPIOSCL, TGPIOMode::GPIOModeInputPullUp);

	const unsigned nIdleSDA = SDA.Read();
	const unsigned nIdleSCL = SCL.Read();

	// START: SDA falls while SCL is high.
	BBDriveLow(SDA);
	BB_DELAY();
	BBDriveLow(SCL);
	BB_DELAY();

	// Address + write bit, MSB first.
	const u8 ucByte = static_cast<u8>(nAddress << 1);
	for (int nBit = 7; nBit >= 0; --nBit)
	{
		if (ucByte & (1 << nBit))
			BBRelease(SDA);
		else
			BBDriveLow(SDA);

		BB_DELAY();
		BBRelease(SCL);
		BB_DELAY();
		BBDriveLow(SCL);
		BB_DELAY();
	}

	// Ninth clock: release SDA and sample the slave's acknowledgement.
	BBRelease(SDA);
	BB_DELAY();
	BBRelease(SCL);
	BB_DELAY();
	const unsigned nAck = SDA.Read();
	BBDriveLow(SCL);
	BB_DELAY();

	// STOP: SDA rises while SCL is high.
	BBDriveLow(SDA);
	BB_DELAY();
	BBRelease(SCL);
	BB_DELAY();
	BBRelease(SDA);
	BB_DELAY();

	CString Line;
	Line.Format("bbprobe 0x%02x: idle SDA=%u SCL=%u, ACK bit=%u -> %s\r\n",
		    nAddress, nIdleSDA, nIdleSCL, nAck,
		    nAck == 0 ? "ACKED (device present)" : "no ACK");
	rResponse.Append(static_cast<const char*>(Line));

	// Hand the pins back to the I2C peripheral.
	CGPIOPin SDAAlt(GPIOSDA, TGPIOMode::GPIOModeAlternateFunction3);
	SDAAlt.SetPullMode(GPIOPullModeUp);
	CGPIOPin SCLAlt(GPIOSCL, TGPIOMode::GPIOModeAlternateFunction3);
	SCLAlt.SetPullMode(GPIOPullModeUp);
	m_pI2CMaster->Initialize();
}

// Inject MIDI bytes directly into the synth's parser, bypassing every input
// path, to tell "MIDI never arrives" apart from "MIDI arrives but does nothing".
void CDebugServer::CmdMIDI(char* pHex, CString& rResponse)
{
	u8 Bytes[256];
	size_t nCount = 0;

	for (const char* p = pHex; *p && nCount < sizeof(Bytes); )
	{
		u8 nValue = 0;
		unsigned nDigits = 0;
		for (; *p && nDigits < 2; ++p)
		{
			u8 nDigit;
			if      (*p >= '0' && *p <= '9') nDigit = *p - '0';
			else if (*p >= 'a' && *p <= 'f') nDigit = *p - 'a' + 10;
			else if (*p >= 'A' && *p <= 'F') nDigit = *p - 'A' + 10;
			else break;
			nValue = static_cast<u8>(nValue * 16 + nDigit);
			++nDigits;
		}
		if (!nDigits) { ++p; continue; }
		Bytes[nCount++] = nValue;
	}

	if (!nCount)
	{
		rResponse.Append("error: no hex bytes parsed\r\n");
		return;
	}

	m_pMT32Pi->DebugInjectMIDI(Bytes, nCount);

	CString Line;
	Line.Format("injected %u MIDI byte(s) directly into the parser\r\n", static_cast<unsigned>(nCount));
	rResponse.Append(static_cast<const char*>(Line));
}

void CDebugServer::CmdMIDIStat(CString& rResponse)
{
	CString Line;
	Line.Format("UDP MIDI receiver: %s\r\nUDP MIDI packets received: %u\r\nserial MIDI enabled: %s\r\n",
		    m_pMT32Pi->DebugUDPMIDIActive() ? "listening on port 1999" : "NOT RUNNING",
		    m_pMT32Pi->DebugUDPMIDIPackets(),
		    m_pMT32Pi->DebugSerialMIDIEnabled() ? "yes" : "no");
	rResponse.Append(static_cast<const char*>(Line));
}
