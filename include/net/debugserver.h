//
// debugserver.h
//
// mt32-pi - A baremetal MIDI synthesizer for Raspberry Pi
//
// TEMPORARY bring-up tool for the Raspberry Pi 5/500 port. Exposes a small
// UDP command interface so hardware state can be poked at interactively
// instead of through a rebuild-and-reboot cycle. Not for release builds.
//

#ifndef _debugserver_h
#define _debugserver_h

#include <circle/i2cmaster.h>
#include <circle/net/socket.h>
#include <circle/sched/task.h>
#include <circle/string.h>

class CMT32Pi;

class CDebugServer : protected CTask
{
public:
	CDebugServer(CI2CMaster* pI2CMaster, CMT32Pi* pMT32Pi);
	virtual ~CDebugServer() override;

	bool Initialize();

	virtual void Run() override;

private:
	void Execute(char* pCommand, CString& rResponse);

	void CmdGPIO(unsigned nPin, CString& rResponse);
	void CmdProbe(u8 nAddress, CString& rResponse);
	void CmdScan(CString& rResponse);
	void CmdRecover(CString& rResponse);
	void CmdDump(CString& rResponse);
	void CmdBitBangProbe(u8 nAddress, CString& rResponse);
	void CmdMIDI(char* pHex, CString& rResponse);
	void CmdMIDIStat(CString& rResponse);

	CSocket* m_pSocket;
	CI2CMaster* m_pI2CMaster;
	CMT32Pi* m_pMT32Pi;
	char m_Buffer[1024];
};

#endif
