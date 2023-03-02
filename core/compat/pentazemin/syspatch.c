#include <pspsdk.h>
#include <pspsysmem_kernel.h>
#include <psputilsforkernel.h>
#include <pspinit.h>
#include <systemctrl.h>
#include <systemctrl_se.h>
#include <systemctrl_private.h>
#include <globals.h> 
#include "functions.h"
#include "macros.h"
#include "exitgame.h"
#include "adrenaline_compat.h"
#include "rebootconfig.h"
#include "libs/graphics/graphics.h"
#include "kermit.h"

extern STMOD_HANDLER previous;

int is_launcher_mode = 0;
int use_mscache = 0;
int use_highmem = 0;
int skip_logos = 0;
int use_infernocache = 0;
int vshregion = 0;

int (* DisplaySetFrameBuf)(void*, int, int, int) = NULL;

// Return Boot Status
int isSystemBooted(void)
{

    // Find Function
    int (* _sceKernelGetSystemStatus)(void) = (void*)sctrlHENFindFunction("sceSystemMemoryManager", "SysMemForKernel", 0x452E3696);
    
    // Get System Status
    int result = _sceKernelGetSystemStatus();
        
    // System booted
    if(result == 0x20000) return 1;
    
    // Still booting
    return 0;
}

void OnSystemStatusIdle() {
	SceAdrenaline *adrenaline = (SceAdrenaline *)ADRENALINE_ADDRESS;

	initAdrenalineInfo();

	// Set fake framebuffer so that cwcheat can be displayed
	if (adrenaline->pops_mode) {
		DisplaySetFrameBuf((void *)NATIVE_FRAMEBUFFER, PSP_SCREEN_LINE, PSP_DISPLAY_PIXEL_FORMAT_8888, PSP_DISPLAY_SETBUF_NEXTFRAME);
		memset((void *)NATIVE_FRAMEBUFFER, 0, SCE_PSPEMU_FRAMEBUFFER_SIZE);
	} else {
		SendAdrenalineCmd(ADRENALINE_VITA_CMD_RESUME_POPS);
	}
}

// kermit_peripheral's sub_000007CC clone, called by loadexec + 0x0000299C with a0=8 (was a0=7 for fw <210)
// Returns 0 on success
int (* Kermit_driver_4F75AA05)(void* kermit_packet, u32 cmd_mode, u32 cmd, u32 argc, u32 allow_callback, u64 *resp) = NULL;
u64 kermit_flash_load(int cmd)
{
    u8 buf[128];
    u64 resp;
    void *alignedBuf = (void*)ALIGN_64((int)buf + 63);
    sceKernelDcacheInvalidateRange(alignedBuf, 0x40);
    KermitPacket *packet = (KermitPacket *)KERMIT_PACKET((int)alignedBuf);
    u32 argc = 0;
    Kermit_driver_4F75AA05(packet, KERMIT_MODE_PERIPHERAL, cmd, argc, KERMIT_CALLBACK_DISABLE, &resp);
    return resp;
}

int flashLoadPatch(int cmd)
{
    int ret = kermit_flash_load(cmd);
    // Custom handling on loadFlash mode, else nothing
    if ( cmd == KERMIT_CMD_ERROR_EXIT || cmd == KERMIT_CMD_ERROR_EXIT_2 )
    {
        int linked;
        // Wait for flash to load
        sceKernelDelayThread(10000);
        // Load FLASH0.ARK
		RebootConfigARK* reboot_config = sctrlHENGetRebootexConfig(NULL);
		char archive[ARK_PATH_SIZE];
		strcpy(archive, ark_config->arkpath);
		strcat(archive, FLASH0_ARK);
		int fd = sceIoOpen(archive, PSP_O_RDONLY, 0777);
		sceIoRead(fd, reboot_config->flashfs, MAX_FLASH0_SIZE);
		sceIoClose(fd);

        flushCache();
    }
    return ret;
}

u32 findKermitFlashDriver(){
    u32 nids[] = {0x4F75AA05, 0x36666181};
    for (int i=0; i<sizeof(nids)/sizeof(u32) && Kermit_driver_4F75AA05 == NULL; i++){
        Kermit_driver_4F75AA05 = sctrlHENFindFunction("sceKermit_Driver", "sceKermit_driver", nids[i]);
    }
    return Kermit_driver_4F75AA05;
}

int patchKermitPeripheral()
{
    findKermitFlashDriver();
    // Redirect KERMIT_CMD_ERROR_EXIT loadFlash function
    u32 knownnids[2] = { 0x3943440D, 0x0648E1A3 /* 3.3X */ };
    u32 swaddress = 0;
    u32 i;
    for (i = 0; i < 2; i++)
    {
        swaddress = findFirstJAL(sctrlHENFindFunction("sceKermitPeripheral_Driver", "sceKermitPeripheral_driver", knownnids[i]));
        if (swaddress != 0)
            break;
    }
    _sw(JUMP(flashLoadPatch), swaddress);
    _sw(NOP, swaddress+4);
    
    return 0;
}

int sctrlKernelLoadExecVSHWithApitypeWithUMDemu(int apitype, const char *file, struct SceKernelLoadExecVSHParam *param) {
	int k1 = pspSdkSetK1(0);

	if (apitype == 0x141){ // homebrew API
        sctrlSESetBootConfFileIndex(MODE_INFERNO); // force inferno to simulate UMD drive
        sctrlSESetUmdFile(""); // empty UMD drive (makes sceUmdCheckMedium return false)
    }	

	SceModule2 *mod = sceKernelFindModuleByName("sceLoadExec");
	u32 text_addr = mod->text_addr;

	int (* LoadExecVSH)(int apitype, const char *file, struct SceKernelLoadExecVSHParam *param, int unk2) = (void *)text_addr + 0x23D0;

	int res = LoadExecVSH(apitype, file, param, 0x10000);
	pspSdkSetK1(k1);
	return res;
}

void patchLoadExecUMDemu(){
    // highjack SystemControl
    u32 func = K_EXTRACT_IMPORT(&sctrlKernelLoadExecVSHWithApitype);
    _sw(JUMP(sctrlKernelLoadExecVSHWithApitypeWithUMDemu), func);
    _sw(NOP, func+4);
    flushCache();
}

int (*_sceKernelVolatileMemTryLock)(int unk, void **ptr, int *size);
int sceKernelVolatileMemTryLockPatched(int unk, void **ptr, int *size) {
	int res = 0;

	int i;
	for (i = 0; i < 0x10; i++) {
		res = _sceKernelVolatileMemTryLock(unk, ptr, size);
		if (res >= 0)
			break;

		sceKernelDelayThread(100);
	}

	return res;
}

static u8 get_pscode_from_region(int region)
{
	u8 code;

	code = region;
	
	if(code < 12) {
		code += 2;
	} else {
		code -= 11;
	}

	if(code == 2) {
		code = 3;
	}

	printk("%s: region %d code %d\n", __func__, region, code);

	return code;
}

int (* _sceChkregGetPsCode)(u8 *pscode);
int sceChkregGetPsCodePatched(u8 *pscode) {
	int res = _sceChkregGetPsCode(pscode);

	pscode[0] = 0x01;
	pscode[1] = 0x00;
	pscode[3] = 0x00;
	pscode[4] = 0x01;
	pscode[5] = 0x00;
	pscode[6] = 0x01;
	pscode[7] = 0x00;

	if (vshregion)
		pscode[2] = get_pscode_from_region(vshregion);

	return res;
}

int sceUmdRegisterUMDCallBackPatched(int cbid) {
	int k1 = pspSdkSetK1(0);
	int res = sceKernelNotifyCallback(cbid, PSP_UMD_NOT_PRESENT);
	pspSdkSetK1(k1);
	return res;
}

int (* sceMeAudio_driver_C300D466)(int codec, int unk, void *info);
int sceMeAudio_driver_C300D466_Patched(int codec, int unk, void *info) {
	int res = sceMeAudio_driver_C300D466(codec, unk, info);

	if (res < 0 && codec == 0x1002 && unk == 2)
		return 0;

	return res;
}

int memcmp_patched(const void *b1, const void *b2, size_t len) {
	u32 tag = 0x4C9494F0;

	if (memcmp(&tag, b2, len) == 0) {
		static u8 kernel661_keys[0x10] = { 0x76, 0xF2, 0x6C, 0x0A, 0xCA, 0x3A, 0xBA, 0x4E, 0xAC, 0x76, 0xD2, 0x40, 0xF5, 0xC3, 0xBF, 0xF9 };
		memcpy((void *)0xBFC00220, kernel661_keys, sizeof(kernel661_keys));
		return 0;
	}

	return memcmp(b1, b2, len);
}

void PatchMemlmd() {
	SceModule2 *mod = sceKernelFindModuleByName("sceMemlmd");
	u32 text_addr = mod->text_addr;
	u32 text_size = mod->text_size;

	// Allow 6.61 kernel modules
	MAKE_CALL(text_addr + 0x2C8, memcmp_patched);
	
	flushCache();
}

int ReadFile(char *file, void *buf, int size) {
	SceUID fd = sceIoOpen(file, PSP_O_RDONLY, 0);
	if (fd < 0)
		return fd;

	int read = sceIoRead(fd, buf, size);

	sceIoClose(fd);
	return read;
}

int sceResmgrDecryptIndexPatched(void *buf, int size, int *retSize) {
	int k1 = pspSdkSetK1(0);
	*retSize = ReadFile("flash0:/vsh/etc/version.txt", buf, size);
	pspSdkSetK1(k1);
	return 0;
}

int sceKernelSuspendThreadPatched(SceUID thid) {
	SceKernelThreadInfo info;
	info.size = sizeof(SceKernelThreadInfo);
	if (sceKernelReferThreadStatus(thid, &info) == 0) {
		if (strcmp(info.name, "popsmain") == 0) {
			SendAdrenalineCmd(ADRENALINE_VITA_CMD_PAUSE_POPS);
		}
	}

	return sceKernelSuspendThread(thid);
}

int sceKernelResumeThreadPatched(SceUID thid) {
	SceKernelThreadInfo info;
	info.size = sizeof(SceKernelThreadInfo);
	if (sceKernelReferThreadStatus(thid, &info) == 0) {
		if (strcmp(info.name, "popsmain") == 0) {
			SendAdrenalineCmd(ADRENALINE_VITA_CMD_RESUME_POPS);
		}
	}

	return sceKernelResumeThread(thid);
}

void patch_GameBoot(SceModule2* mod){
    u32 p1 = 0;
    u32 p2 = 0;
    int patches = 2;
    for (u32 addr=mod->text_addr; addr<mod->text_addr+mod->text_size && patches; addr+=4){
        u32 data = _lw(addr);
        if (data == 0x2C43000D){
            p1 = addr-36;
            patches--;
        }
        else if (data == 0x27BDFF20 && _lw(addr-4) == 0x27BD0040){
            p2 = addr-24;
            patches--;
        }
    }
    _sw(JAL(p1), p2);
    _sw(0x24040002, p2 + 4);
}

int InitUsbPatched() {
	return SendAdrenalineCmd(ADRENALINE_VITA_CMD_START_USB);
}

int ShutdownUsbPatched() {
	return SendAdrenalineCmd(ADRENALINE_VITA_CMD_STOP_USB);
}

int GetUsbStatusPatched() {	
	int state = SendAdrenalineCmd(ADRENALINE_VITA_CMD_GET_USB_STATE);

	if (state & 0x20)
		return 1; // Connected

	return 2; // Not connected
}

u32 MakeSyscallStub(void *function) {
	SceUID block_id = sceKernelAllocPartitionMemory(PSP_MEMORY_PARTITION_USER, "", PSP_SMEM_High, 2 * sizeof(u32), NULL);
	u32 stub = (u32)sceKernelGetBlockHeadAddr(block_id);
	_sw(0x03E00008, stub);
	_sw(0x0000000C | (sceKernelQuerySystemCall(function) << 6), stub + 4);
	return stub;
}

void patch_VshMain(SceModule2* mod){
	u32 text_addr = mod->text_addr;

	// Dummy usb detection functions
	MAKE_DUMMY_FUNCTION(text_addr + 0x38C94, 0);
	MAKE_DUMMY_FUNCTION(text_addr + 0x38D68, 0);
}

void patch_SysconfPlugin(SceModule2* mod){
	u32 text_addr = mod->text_addr;
	// Dummy all vshbridge usbstor functions
	_sw(0x24020001, text_addr + 0xCD78); // sceVshBridge_ED978848 - vshUsbstorMsSetWorkBuf
	_sw(0x00001021, text_addr + 0xCDAC); // sceVshBridge_EE59B2B7
	_sw(0x00001021, text_addr + 0xCF0C); // sceVshBridge_6032E5EE - vshUsbstorMsSetProductInfo
	_sw(0x00001021, text_addr + 0xD218); // sceVshBridge_360752BF - vshUsbstorMsSetVSHInfo

	// Dummy LoadUsbModules, UnloadUsbModules
	MAKE_DUMMY_FUNCTION(text_addr + 0xCC70, 0);
	MAKE_DUMMY_FUNCTION(text_addr + 0xD2C4, 0);

	// Redirect USB functions
	REDIRECT_FUNCTION(text_addr + 0xAE9C, MakeSyscallStub(InitUsbPatched));
	REDIRECT_FUNCTION(text_addr + 0xAFF4, MakeSyscallStub(ShutdownUsbPatched));
	REDIRECT_FUNCTION(text_addr + 0xB4A0, MakeSyscallStub(GetUsbStatusPatched));
	/*
	MAKE_SYSCALL(mod->text_addr + 0xAE9C, InitUsbPatched);
	MAKE_SYSCALL(mod->text_addr + 0xAFF4, ShutdownUsbPatched);
	MAKE_SYSCALL(mod->text_addr + 0xB4A0, GetUsbStatusPatched);
	*/

	// Ignore wait thread end failure
	_sw(0, text_addr + 0xB264);
}

void settingsHandler(char* path){
    int apitype = sceKernelInitApitype();
	if (strcasecmp(path, "highmem") == 0){ // enable high memory
        if ( (apitype == 0x120 || (apitype >= 0x123 && apitype <= 0x126)) && sceKernelFindModuleByName("sceUmdCache_driver") != NULL){
            // don't allow high memory in UMD when cache is enabled
            return;
        }
        use_highmem = 1;
    }
    else if (strcasecmp(path, "mscache") == 0){
        use_mscache = 1; // enable ms cache for speedup
    }
    else if (strcasecmp(path, "launcher") == 0){ // replace XMB with custom launcher
        is_launcher_mode = 1;
    }
    else if (strcasecmp(path, "infernocache") == 0){
        if (apitype == 0x123 || apitype == 0x125 || (apitype >= 0x112 && apitype <= 0x115))
            use_infernocache = 1;
    }
    else if (strcasecmp(path, "skiplogos") == 0){
        skip_logos = 1;
    }
	else if (strncasecmp(path, "fakeregion_", 11) == 0){
        int r = atoi(path+11);
        vshregion = r;
    }
}

void AdrenalineOnModuleStart(SceModule2 * mod){

    // System fully booted Status
    static int booted = 0;

    if(strcmp(mod->modname, "sceDisplay_Service") == 0)
    {
        // can use screen now
        DisplaySetFrameBuf = (void*)sctrlHENFindFunction("sceDisplay_Service", "sceDisplay", 0x289D82FE);
        goto flush;
    }

	// Patch Kermit Peripheral Module to load flash0
    if(strcmp(mod->modname, "sceKermitPeripheral_Driver") == 0)
    {
        patchKermitPeripheral();
        goto flush;
    }

	if (strcmp(mod->modname, "sceLowIO_Driver") == 0) {

		// Protect pops memory
		if (sceKernelInitKeyConfig() == PSP_INIT_KEYCONFIG_POPS) {
			sceKernelAllocPartitionMemory(6, "", PSP_SMEM_Addr, 0x80000, (void *)0x09F40000);
			memset((void *)0x49F40000, 0, 0x80000);
		}

		memset((void *)0xABCD0000, 0, 0x1B0);

		PatchLowIODriver2(mod->text_addr);
        goto flush;
    }

    if (strcmp(mod->modname, "sceLoadExec") == 0) {
		PatchLoadExec(mod->text_addr, mod->text_size);
        goto flush;
	}

	if (strcmp(mod->modname, "scePower_Service") == 0) {
		PatchPowerService(mod->text_addr);
		PatchPowerService2(mod->text_addr);
        goto flush;
	}
    
	if (strcmp(mod->modname, "sceChkreg") == 0) {
		MAKE_DUMMY_FUNCTION(sctrlHENFindFunction("sceChkreg", "sceChkreg_driver", 0x54495B19), 1);
    	HIJACK_FUNCTION(sctrlHENFindFunction("sceChkreg", "sceChkreg_driver", 0x59F8491D), sceChkregGetPsCodePatched, _sceChkregGetPsCode);
        goto flush;
    }

    if (strcmp(mod->modname, "sceMesgLed") == 0) {
		REDIRECT_FUNCTION(sctrlHENFindFunction("sceMesgLed", "sceResmgr_driver", 0x9DC14891), sceResmgrDecryptIndexPatched);
		goto flush;
	}

	if (strcmp(mod->modname, "sceUmd_driver") == 0) {
		REDIRECT_FUNCTION(mod->text_addr + 0xC80, sceUmdRegisterUMDCallBackPatched);
		goto flush;
	}

	if(strcmp(mod->modname, "sceMeCodecWrapper") == 0) {
		HIJACK_FUNCTION(sctrlHENFindFunction(mod->modname, "sceMeAudio_driver", 0xC300D466), sceMeAudio_driver_C300D466_Patched, sceMeAudio_driver_C300D466);
		goto flush;
	}

    if (strcmp(mod->modname, "sceUtility_Driver") == 0) {
		PatchUtility();
        goto flush;
	}

    if (strcmp(mod->modname, "sceImpose_Driver") == 0) {
		PatchImposeDriver(mod->text_addr);
        goto flush;
	}

    if (strcmp(mod->modname, "sceNpSignupPlugin_Module") == 0) {
		// ImageVersion = 0x10000000
		_sw(0x3C041000, mod->text_addr + 0x38CBC);
		goto flush;
	}

    if (strcmp(mod->modname, "sceVshNpSignin_Module") == 0) {
		// Kill connection error
		_sw(0x10000008, mod->text_addr + 0x6CF4);
		// ImageVersion = 0x10000000
		_sw(0x3C041000, mod->text_addr + 0x96C4);
		goto flush;
	}

	if(strcmp(mod->modname, "sceVshBridge_Driver") == 0) {
		if (skip_logos){
            // patch GameBoot
            hookImportByNID(mod, "sceDisplay_driver", 0x3552AB11, 0);
        }
        goto flush;
	}

	if(strcmp(mod->modname, "game_plugin_module") == 0) {
		if (skip_logos) {
		    patch_GameBoot(mod);
	    }
        goto flush;
	}

	if (strcmp(mod->modname, "sysconf_plugin_module") == 0){
		patch_SysconfPlugin(mod);
		goto flush;
	}

	if (strcmp(mod->modname, "vsh_module") == 0) {
		patch_VshMain(mod);
		goto flush;
	}

    if (strcmp(mod->modname, "sceSAScore") == 0) {
		PatchSasCore();
        goto flush;
	}
    
    if (strcmp(mod->modname, "Legacy_Software_Loader") == 0){
        // Remove patch of sceKernelGetUserLevel on sceLFatFs_Driver
        _sw(NOP, mod->text_addr + 0x1140);
        goto flush;
    }

	if (strcmp(mod->modname, "CWCHEATPRX") == 0) {
		if (sceKernelInitKeyConfig() == PSP_INIT_KEYCONFIG_POPS) {
			MAKE_JUMP(sctrlHENFindImport(mod->modname, "ThreadManForKernel", 0x9944F31F), sceKernelSuspendThreadPatched);
			MAKE_JUMP(sctrlHENFindImport(mod->modname, "ThreadManForKernel", 0x75156E8F), sceKernelResumeThreadPatched);
			goto flush;
		}
	}

    // VLF Module Patches
    if(strcmp(mod->modname, "VLF_Module") == 0)
    {
        // Patch VLF Module
        patchVLF(mod);
        // Exit Handler
        goto flush;
    }

	// load and process settings file
    if(strcmp(mod->modname, "sceMediaSync") == 0)
    {
        //PatchMediaSync(mod->text_addr);
        loadSettings(&settingsHandler);
		if (is_launcher_mode){
			strcpy(ark_config->launcher, ARK_MENU); // set CFW in launcher mode
		}
		else{
			ark_config->launcher[0] = 0; // disable launcher mode
		}
		// apply extra memory patch
		if (use_highmem) unlockVitaMemory();
		// enable inferno cache
		if (use_infernocache){
			int (*CacheInit)(int, int, int) = sctrlHENFindFunction("PRO_Inferno_Driver", "inferno_driver", 0x8CDE7F95);
			if (CacheInit){
				CacheInit(32 * 1024, 32, (use_highmem)?2:9); // 2MB cache for PS Vita
			}
            }
		sctrlHENSetArkConfig(ark_config);
        goto flush;
    }
       
    // Boot Complete Action not done yet
    if(booted == 0)
    {
        // Boot is complete
        if(isSystemBooted())
        {
            // Initialize Memory Stick Speedup Cache
            if (use_mscache) msstorCacheInit("ms", 8 * 1024);

            // patch bug in ePSP volatile mem
            _sceKernelVolatileMemTryLock = (void *)sctrlHENFindFunction("sceSystemMemoryManager", "sceSuspendForUser", 0xA14F40B2);
            sctrlHENPatchSyscall((u32)_sceKernelVolatileMemTryLock, sceKernelVolatileMemTryLockPatched);

			// Adrenaline patches
			OnSystemStatusIdle();

            // Boot Complete Action done
            booted = 1;
            goto flush;
        }
    }

flush:
    flushCache();

exit:
       // Forward to previous Handler
    if(previous) previous(mod);
}

int (*prev_start)(int modid, SceSize argsize, void * argp, int * modstatus, SceKernelSMOption * opt) = NULL;
int StartModuleHandler(int modid, SceSize argsize, void * argp, int * modstatus, SceKernelSMOption * opt){

    SceModule2* mod = (SceModule2*) sceKernelFindModuleByUID(modid);

	if (skip_logos && mod != NULL && ark_config->launcher[0] == 0 && 0 == strcmp(mod->modname, "vsh_module") ) {
		u32* vshmain_args = oe_malloc(1024);

		memset(vshmain_args, 0, 1024);

		if(argp != NULL && argsize != 0 ) {
			memcpy( vshmain_args , argp ,  argsize);
		}

		vshmain_args[0] = 1024;
		vshmain_args[1] = 0x20;
		vshmain_args[16] = 1;

		int ret = sceKernelStartModule(modid, 1024, vshmain_args, modstatus, opt);
		oe_free(vshmain_args);

		return ret;
	}

    // forward to previous or default StartModule
    if (prev_start) return prev_start(modid, argsize, argp, modstatus, opt);
    return -1;
}

void AdrenalineSysPatch(){
	// Patch stuff
    SceModule2* loadcore = patchLoaderCore();
    PatchIoFileMgr();
    PatchMemlmd();
	// patch loadexec to use inferno for UMD drive emulation (needed for some homebrews to load)
    patchLoadExecUMDemu();
	// initialize Adrenaline Layer
    initAdrenaline();
}
