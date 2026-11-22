import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CAM = ROOT / "engines" / "camreplace"


class FusionSafetyGuards(unittest.TestCase):
    def read(self, relative):
        return (CAM / relative).read_text(encoding="utf-8")

    def test_enhanced_host_never_disables_selinux(self):
        phone_ops = self.read("host/src/device/PhoneOps.cpp")
        self.assertNotIn("setenforce 0", phone_ops)
        self.assertNotIn("chmod 0777", phone_ops)
        self.assertNotIn("chmod 0666", phone_ops)
        self.assertIn("chmod 0600", phone_ops)

    def test_camera_waits_on_fence_and_uses_official_buffer_conversion(self):
        hook = self.read("android/camhook/src/hook_main.cpp")
        self.assertIn("ANativeWindowBuffer_getHardwareBuffer", hook)
        self.assertIn("dup(fence)", hook)
        self.assertIn("overwrite_nv21(anb, fence)", hook)
        self.assertNotIn("g_ahb_lockPlanes(ahb, kAhbUsageCpuRW, -1", hook)

    def test_virtual_microphone_is_opt_in_and_fail_closed_when_active(self):
        ui = self.read("host/src/ui/MainWindow.cpp")
        audio = self.read("android/audhook/src/audhook_main.cpp")
        controller = self.read("host/src/source/FeedController.cpp")
        self.assertIn("Virtual microphone: OFF (phone microphone is used)", ui)
        self.assertIn("Enable virtual microphone", ui)
        self.assertIn("bool sound_replacing = false", ui)
        self.assertIn("dev.arm_audio(serial, cb)", controller)
        self.assertIn("dev.disarm_audio(serial, cb)", controller)
        self.assertIn("memset(buffer, 0, got)", audio)
        active_check = audio.index("channel_state) != CR_CHANNEL_STATE_READY")
        clear = audio.index("memset(buffer, 0, got)")
        sanity = audio.index("audio_header_sane(g_shm.header, g_shm.size)", clear)
        self.assertLess(active_check, clear)
        self.assertLess(clear, sanity)

    def test_camera_start_does_not_implicitly_arm_audio(self):
        controller = self.read("host/src/source/FeedController.cpp")
        start_video = controller.split("void FeedController::start_replace_camera", 1)[1]
        start_video = start_video.split("void FeedController::stop_replace_camera", 1)[0]
        self.assertNotIn("arm_audio", start_video)

        device_stack = self.read("host/src/device/DeviceStack.cpp")
        preload = device_stack.split("bool DeviceStack::load_hooks", 1)[1]
        preload = preload.split("bool DeviceStack::unload_hooks", 1)[0]
        self.assertNotIn("HostProc::AudioServer", preload)
        self.assertNotIn("cr_audhook_init", preload)

    def test_injector_does_not_accept_arbitrary_null_fault_as_return(self):
        ptrace = self.read("android/injector/src/ptrace_arm64.cpp")
        self.assertNotIn("after.pc == 0 || fault_addr == 0", ptrace)
        self.assertIn("after.pc == 0", ptrace)

    def test_enhanced_engine_is_present_under_gpl(self):
        self.assertTrue((CAM / "LICENSE").is_file())
        self.assertIn("GNU GENERAL PUBLIC LICENSE", (ROOT / "LICENSE").read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
