import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CAM = ROOT / "engines" / "camreplace"


class FusionSafetyGuards(unittest.TestCase):
    def read(self, relative):
        return (CAM / relative).read_text(encoding="utf-8")

    def test_native_host_never_disables_selinux(self):
        phone_ops = self.read("host/src/device/PhoneOps.cpp")
        self.assertNotIn("setenforce 0", phone_ops)
        self.assertNotIn("chmod 0777", phone_ops)
        self.assertNotIn("chmod 0666", phone_ops)
        self.assertIn("chmod 0600", phone_ops)
        launch = phone_ops.split("bool PhoneOps::launch_feed", 1)[1]
        launch = launch.split("bool PhoneOps::wait_feed_ready", 1)[0]
        self.assertLess(launch.index("safe_remote_basename"), launch.index("prepare owner-restricted"))
        inject = phone_ops.split("bool PhoneOps::inject_lib", 1)[1]
        inject = inject.split("void PhoneOps::restart_services", 1)[0]
        self.assertLess(inject.index("unexpected library, init symbol, or feed kind"),
                        inject.index("hook_loaded(target)"))

    def test_camera_waits_on_fence_and_uses_official_buffer_conversion(self):
        hook = self.read("android/camhook/src/hook_main.cpp")
        self.assertIn("ANativeWindowBuffer_getHardwareBuffer", hook)
        self.assertIn("dup(fence)", hook)
        self.assertIn("overwrite_nv21(anb, fence)", hook)
        self.assertNotIn("g_ahb_lockPlanes(ahb, kAhbUsageCpuRW, -1", hook)
        blob = hook.split("static bool lock_blob", 1)[1]
        blob = blob.split("static bool overwrite_blob_jpeg", 1)[0]
        self.assertIn("g_ahb_lock && g_ahb_unlock && g_get_hardware_buffer", blob)
        self.assertIn("lock_fence, nullptr, &vaddr", blob)
        self.assertIn("if (rc == 0)\n        g_ahb_unlock", blob)
        self.assertGreaterEqual(blob.count("if (rc == 0)\n      gbm_call_unlock(handle)"), 2)
        self.assertIn("struct BlobLockGuard", hook)
        self.assertIn("BlobLockGuard blob_lock", hook)
        photo = hook.split("static bool overwrite_blob_jpeg", 1)[1]
        photo = photo.split("static cr::camhook::ReplacementOutcome overwrite_nv21", 1)[0]
        self.assertIn("if (H != 1)", photo)
        self.assertIn("const size_t buf_capacity = (size_t) W;", photo)
        self.assertNotIn("(size_t) W * (size_t) H", photo)
        self.assertIn("static bool frame_is_fresh", hook)
        self.assertIn("kFrameStaleNs", hook)
        self.assertGreaterEqual(hook.count("frame_is_fresh(source)"), 2)
        self.assertIn("chroma_delta != 1u", hook)
        self.assertIn("p0.pixelStride != 1", hook)
        self.assertIn("p2.pixelStride != 2", hook)
        pinned = hook.split("bool pin_current", 1)[1]
        pinned = pinned.split("static std::atomic<bool> g_installed", 1)[0]
        self.assertIn("raw_n == 0 || raw_n > CR_FEED_NUM_SLOTS", pinned)
        self.assertIn("header->num_slots", pinned)
        self.assertIn("min_slot > slot_size", pinned)

    def test_virtual_microphone_is_opt_in_and_fail_closed_when_active(self):
        ui = self.read("host/src/ui/MainWindow.cpp")
        audio = self.read("android/audhook/src/audhook_main.cpp")
        controller = self.read("host/src/source/FeedController.cpp")
        self.assertIn("Virtual microphone: OFF (phone microphone is used)", ui)
        self.assertIn("Enable virtual microphone", ui)
        self.assertIn("READY (OBS audio is used)", ui)
        self.assertIn("STALLED (check OBS audio/USB)", ui)
        self.assertIn("UNSUPPORTED (AAC decoder/format)", ui)
        self.assertIn("std::atomic_bool sound_replacing{false}", ui)
        self.assertIn("dev.arm_audio(serial, cb)", controller)
        self.assertIn("dev.disarm_audio(serial, cb)", controller)
        self.assertIn("memset(buffer, 0, got)", audio)
        self.assertIn("proxy_streamin_read_u32", audio)
        self.assertIn("unsigned int* read_out", audio)
        self.assertIn("EPvjPj\").c_str(), narrow", audio)
        self.assertIn("if (got > bytes)", audio)
        self.assertIn("std::min(static_cast<size_t>(*read_out)", audio)
        self.assertIn("bytes_after_first", audio)
        self.assertNotIn("p.period_bytes != 0", audio)
        self.assertIn("(l + r) / 2", audio)
        active_check = audio.index("gate.channel_state != CR_CHANNEL_STATE_READY")
        clear = audio.index("memset(buffer, 0, got)")
        sanity = audio.index("AudioHeaderSnapshot current", clear)
        self.assertLess(active_check, clear)
        self.assertLess(clear, sanity)

    def test_async_ui_and_device_sessions_are_isolated(self):
        ui = self.read("host/src/ui/MainWindow.cpp")
        header = self.read("host/src/device/DeviceStack.h")
        stack = self.read("host/src/device/DeviceStack.cpp")
        controller = self.read("host/src/source/FeedController.cpp")
        host_header = self.read("host/src/source/HostStack.h")
        app = self.read("host/src/app/App.cpp")
        self.assertIn("std::atomic_uint64_t audio_operation", ui)
        self.assertNotIn(".detach()", ui)
        self.assertIn("device_sound_player().stop()", ui)
        self.assertIn("state().audio_operation.load() == operation", ui)
        self.assertIn("ImGui::BeginDisabled(device_locked)", ui)
        self.assertIn("std::unordered_set<std::string> prepared_serials_", header)
        self.assertIn("prepared_serials_.contains(serial)", stack)
        self.assertIn("std::mutex g_lifecycle_mu", controller)
        self.assertIn("std::lock_guard<std::mutex> lifecycle(g_lifecycle_mu)", controller)
        self.assertIn("std::string session_serial_", host_header)
        self.assertIn("class OperationExecutor", controller)
        self.assertIn("operation_executor().stop_and_join()", controller)
        self.assertNotIn(".detach()", controller)
        self.assertIn("touched_serials_", header)
        self.assertIn("session_serials()", controller)
        self.assertNotIn("DeviceScanner::instance().snapshot()", controller)
        self.assertIn("FeedController::shutdown_all()", app)

    def test_secure_channel_requires_ephemeral_ticket_proof(self):
        session = self.read("common/secure_channel/SecureSession.cpp")
        client = self.read("host/src/transport/TcpFeedClient.cpp")
        self.assertIn("verify_local_ticket(view, bytes_of(shared))", session)
        self.assertIn("sign_local_ticket", client)
        self.assertNotIn("all_zero(view.signature", session)

    def test_shutdown_and_device_commands_are_bounded(self):
        rtmp = self.read("host/src/source/RtmpServer.cpp")
        rtmp_header = self.read("host/src/source/RtmpServer.h")
        adb_header = self.read("host/src/device/AdbClient.h")
        reader = self.read("android/feed/src/secure_channel_reader.cpp")
        self.assertIn("client_sock_", rtmp_header)
        self.assertIn("::shutdown(client, SD_BOTH)", rtmp)
        self.assertIn("int timeout_ms = 30000", adb_header)
        adb = self.read("host/src/device/AdbClient.cpp")
        self.assertIn("PROC_THREAD_ATTRIBUTE_HANDLE_LIST", adb)
        self.assertGreaterEqual(adb.count("InheritedHandleList handle_list"), 2)
        self.assertIn("kMaxCapturedBytes", adb)
        self.assertIn("CancelSynchronousIo", adb)
        self.assertIn("notify_line_noexcept", adb)
        self.assertIn("SO_RCVTIMEO", reader)
        self.assertIn("SO_SNDTIMEO", reader)
        socket_wait = self.read("android/feed/src/interruptible_socket.h")
        self.assertIn("::poll(&descriptor, 1, 250)", socket_wait)
        for name in ("tcp_h264_feed.cpp", "tcp_nv21_feed.cpp", "tcp_pcm_feed.cpp"):
            feed = self.read("android/feed/src/" + name)
            self.assertIn("accept_interruptible", feed)

    def test_media_workers_validate_buffers_and_contain_failures(self):
        preview = self.read("host/src/render/H264PreviewDecoder.cpp")
        audio = self.read("host/src/source/AudioPump.cpp")
        phone_decoder = self.read("android/feed/src/tcp_h264_feed.cpp")
        self.assertIn("y_bytes + uv_bytes <= cl", preview)
        self.assertIn("decoder worker stopped", preview)
        self.assertIn("len > MAXDWORD", preview)
        self.assertIn("bool AudioPump::ensure_transport_()", audio)
        self.assertGreaterEqual(audio.count("if (!running_.load())"), 3)
        self.assertIn("running_.store(true)", audio)
        self.assertIn("csd_len > 64", audio)
        self.assertGreaterEqual(audio.count("in_buf->Unlock();"), 4)
        self.assertIn("pts_anchor_us = ph.pts_us", phone_decoder)
        self.assertIn("wall_anchor_ms = arrived", phone_decoder)
        self.assertIn('~ShmOut() { shutdown(OBF("scope exit").c_str()); }', phone_decoder)
        pcm_feed = self.read("android/feed/src/tcp_pcm_feed.cpp")
        self.assertIn("~AudioShm()", pcm_feed)
        self.assertIn("revoke_();", pcm_feed)
        feed_main = self.read("android/feed/src/feed_main.cpp")
        self.assertIn("catch (const std::bad_alloc&)", feed_main)

    def test_secure_noexcept_paths_report_memory_pressure(self):
        session = self.read("common/secure_channel/SecureSession.cpp")
        status = self.read("common/secure_channel/Status.h")
        self.assertIn("ResourceExhausted", status)
        self.assertIn("catch (const std::bad_alloc&)", session)
        derive = session.split("Status SecureSession::derive_material", 1)[1]
        derive = derive.split("Status SecureSession::nonce_for", 1)[0]
        self.assertNotIn("std::vector", derive)
        self.assertIn("shared.size != kKeyBytes", derive)

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

        prepare = device_stack.split("bool DeviceStack::prepare_artifacts_", 1)[1]
        prepare = prepare.split("bool DeviceStack::cleanup_device_", 1)[0]
        self.assertNotIn("restart_services", prepare)
        self.assertNotIn("prepare_camera_hook_", device_stack)
        self.assertNotIn("prepare_audio_hook_", device_stack)
        self.assertIn("refresh_service = !camera_hook_serials_.contains(serial)", preload)
        self.assertIn("camera_hook_serials_.insert(serial)", preload)
        arm_audio = device_stack.split("bool DeviceStack::arm_audio", 1)[1]
        arm_audio = arm_audio.split("void DeviceStack::disarm_audio", 1)[0]
        self.assertIn("HostProc::AudioServer", arm_audio)
        self.assertIn("refresh_service = !audio_hook_serials_.contains(serial)", arm_audio)

        phone_ops = self.read("host/src/device/PhoneOps.cpp")
        inject = phone_ops.split("bool PhoneOps::inject_lib", 1)[1]
        inject = inject.split("bool PhoneOps::restart_services", 1)[0]
        self.assertIn('restart_services({"cameraserver"}', inject)
        self.assertIn('restart_services({"audioserver"}', inject)
        self.assertIn("restore_camera_clients();", inject)
        self.assertGreaterEqual(
            device_stack.count("ops.hook_loaded(HostProc::CameraServer)"), 2
        )
        self.assertGreaterEqual(
            device_stack.count("ops.hook_loaded(HostProc::AudioServer)"), 2
        )
        self.assertIn("relaunch_remembered_camera_clients", device_stack)

    def test_injector_does_not_accept_arbitrary_null_fault_as_return(self):
        ptrace = self.read("android/injector/src/ptrace_arm64.cpp")
        self.assertNotIn("after.pc == 0 || fault_addr == 0", ptrace)
        self.assertIn("after.pc == 0", ptrace)

    def test_camera_hook_never_guesses_unknown_cpp_signature(self):
        hook = self.read("android/camhook/src/hook_main.cpp")
        dispatch = self.read("android/camhook/src/HookDispatch.h")
        primary = hook.split("static int try_hook_primary()", 1)[1]
        self.assertNotIn("try_prefix", primary)
        self.assertNotIn("shadowhook_hook_sym_addr", primary)
        self.assertNotIn("try_hook_fallback", hook)
        self.assertNotIn("proxy_rbcl", hook)
        self.assertIn("Only exact, reviewed mangled signatures", primary)
        self.assertIn("using qbtc_surfaces_t = std::int32_t", hook)
        self.assertIn("using qbtc_legacy_t = std::int32_t", hook)
        self.assertIn("return status", dispatch)
        self.assertIn("const std::vector<std::size_t>& surface_ids", hook)
        self.assertTrue((ROOT / "tests/native/hook_dispatch_test.cpp").exists())

    def test_ring_reconnect_and_publication_preserve_readers(self):
        shm = self.read("android/feed/src/shm.h")
        feed = self.read("android/feed/src/tcp_h264_feed.cpp")
        publish = shm.split("static inline uint64_t cr_feed_publish", 1)[1]
        publish = publish.split("#define CR_FEED_MAX_WIDTH", 1)[0]
        self.assertIn("cr_feed_recover_dead_writer", shm)
        self.assertIn("cr_feed_ownership_layout_valid", shm)
        self.assertIn("flock(fd, LOCK_EX | LOCK_NB)", feed)
        self.assertIn("ownership_compatible = cr_feed_ownership_layout_valid(hd)", feed)
        ownership = shm.split("static inline int cr_feed_ownership_layout_valid", 1)[1]
        ownership = ownership.split("static inline void cr_feed_reset_slot_states", 1)[0]
        self.assertNotIn("magic", ownership)
        # write_index is published before frame_counter. Readers load the
        # sequence first, so acquiring a new sequence also orders its index.
        # The slot writer token remains held until both fields are final.
        write_store = publish.index("atomic_store_explicit(&h->write_index")
        sequence_store = publish.index("atomic_store_explicit(&h->frame_counter")
        release_store = publish.index("atomic_store_explicit(&h->slot_state")
        self.assertLess(write_store, sequence_store)
        self.assertLess(sequence_store, release_store)

    def test_parsers_and_audio_clock_have_native_regressions(self):
        rtmp_h = self.read("host/src/source/RtmpServer.h")
        rtmp = self.read("host/src/source/RtmpServer.cpp")
        audio = self.read("android/audhook/src/audhook_main.cpp")
        self.assertIn("std::uintptr_t client", rtmp_h)
        self.assertNotIn("serve_client_((int) client)", rtmp)
        self.assertIn("kMaxRtmpMessageBytes", rtmp)
        self.assertIn("kMaxBufferedRtmpBytes", rtmp)
        self.assertIn("avcc_nalu_length_size", rtmp)
        self.assertIn("PcmResampleClock resample_clock", audio)
        for name in ("avcc_test.cpp", "pcm_resampler_test.cpp", "frame_layout_test.cpp"):
            self.assertTrue((ROOT / "tests" / "native" / name).exists())

        direct = self.read("android/feed/src/rtmp_h264_feed.cpp")
        threaded = self.read("android/feed/src/tcp_h264_feed_v2.cpp")
        queues = self.read("android/feed/src/feed_queues.h")
        self.assertIn("kMaxRtmpMessageBytes", direct)
        self.assertIn("kMaxBufferedRtmpBytes", direct)
        self.assertIn("kMaxChunkStreams", direct)
        self.assertIn("body.size() < 5", direct)
        self.assertIn("parse_avc_decoder_config", direct)
        self.assertIn("avcc_access_unit_to_annexb", direct)
        self.assertNotIn("defaulting to %dx%d", direct)
        self.assertIn("valid_codec_window", threaded)
        self.assertNotIn("memset(d.nv21_scratch", threaded)
        self.assertIn("if (closed_)", queues)

    def test_audio_publish_orders_data_before_ready_and_honors_codec_window(self):
        pcm = self.read("android/feed/src/tcp_pcm_feed.cpp")
        direct_aac = self.read("android/feed/src/aac_decode_pipe.h")
        audio_abi = self.read("android/feed/src/audio_shm.h")
        audhook = self.read("android/audhook/src/audhook_main.cpp")

        stream = pcm.split("int run_stream", 1)[1].split("} // namespace", 1)[0]
        self.assertLess(stream.index("write_pos"), stream.index("channel_state"))
        self.assertIn("memory_order_release", stream)

        writer = direct_aac.split("void write_to_ring", 1)[1]
        self.assertLess(writer.index("write_pos"), writer.index("channel_state"))
        self.assertIn("out_buf + info.offset", direct_aac)
        self.assertIn("cap - static_cast<size_t>(info.offset)", direct_aac)
        self.assertIn("flock(fd, LOCK_EX | LOCK_NB)", pcm)
        self.assertIn("flock(fd, LOCK_EX | LOCK_NB)", direct_aac)
        self.assertIn("cr_audio_next_generation", audio_abi)
        self.assertIn("cr_audio_begin_write", audio_abi)
        self.assertIn("cr_audio_write_sequence_stable", audio_abi)
        self.assertNotIn("atomic_fetch_add(&h->generation", pcm)
        self.assertNotIn("atomic_fetch_add(&h->generation", direct_aac)
        self.assertIn("input packet too large", direct_aac)
        self.assertIn("bytes -= bytes % frame_bytes", direct_aac)
        self.assertIn("copy -= copy % frame_bytes", audhook)
        self.assertIn("if (rp > wp || wp - rp > ring)", audhook)
        self.assertNotIn("(int16_t*) dst", audhook)
        self.assertIn("store_s16le", audhook)
        self.assertIn("cr_audio_write_sequence_stable", audhook)
        self.assertIn("audio_header_snapshot", audhook)
        self.assertIn("same_audio_generation", audhook)
        self.assertIn("__atomic_load_n(&h->sample_rate", audhook)
        self.assertNotIn("g_shm.header->sample_rate", audhook)

    def test_release_resources_cannot_silently_be_empty(self):
        cmake = self.read("host/CMakeLists.txt")
        embed = self.read("host/cmake/Embed.cmake")
        workflow = (ROOT / ".github" / "workflows" / "quality.yml").read_text(encoding="utf-8")
        self.assertIn("CR_ALLOW_MISSING_RESOURCES", cmake)
        self.assertIn("required file missing", embed)
        self.assertIn("_plain_size EQUAL 0", embed)
        self.assertIn("build-manifest.json", workflow)
        self.assertIn("Get-FileHash", workflow)
        resources = self.read("host/src/resources/EmbeddedFs.cpp")
        self.assertIn("CREATE_NEW", resources)
        self.assertIn("MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH", resources)
        self.assertIn("PROTECTED_DACL_SECURITY_INFORMATION", resources)
        self.assertIn("resource verification failed after replace", resources)
        self.assertNotIn('tmp += ".tmp";', resources)

    def test_recam_sources_are_in_the_native_pipeline(self):
        source_h = self.read("host/src/source/DesktopSource.h")
        source_cpp = self.read("host/src/source/DesktopSource.cpp")
        host = self.read("host/src/source/HostStack.cpp")
        ui = self.read("host/src/ui/MainWindow.cpp")
        cmake = self.read("host/CMakeLists.txt")
        for kind in ("TestPattern", "FileOrUrl", "Camera"):
            self.assertIn(kind, source_h)
        self.assertIn("CreateProcessW", source_cpp)
        self.assertIn("PROC_THREAD_ATTRIBUTE_HANDLE_LIST", source_cpp)
        self.assertIn("JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE", source_cpp)
        self.assertIn("GetModuleFileNameW", source_cpp)
        self.assertIn("SearchPathW(safe_path.c_str()", source_cpp)
        self.assertNotIn('SearchPathW(nullptr, L"ffmpeg.exe"', source_cpp)
        self.assertIn("DesktopSource::list_cameras", source_cpp)
        self.assertIn("desktop->start(config", host)
        self.assertIn("Video file or URL", ui)
        self.assertIn("Windows camera", ui)
        self.assertIn("src/source/DesktopSource.cpp", cmake)

    def test_late_microphone_opt_in_replays_aac_configuration(self):
        source = self.read("host/src/source/ObsRtmpSource.cpp")
        header = self.read("host/src/source/ObsRtmpSource.h")
        self.assertIn("last_aac_csd_", header)
        self.assertIn("cached = last_aac_csd_", source)
        self.assertIn("audio_.on_aac_csd(cached.data(), cached.size())", source)

    def test_product_exposes_one_release_video_transport(self):
        controller_h = self.read("host/src/source/FeedController.h")
        source_h = self.read("host/src/source/ObsRtmpSource.h")
        phone_h = self.read("host/src/device/PhoneOps.h")
        android_cmake = self.read("android/CMakeLists.txt")
        feed_main = self.read("android/feed/src/feed_main.cpp")
        ui = self.read("host/src/ui/MainWindow.cpp")
        self.assertNotIn("enum class Transport", controller_h)
        self.assertNotIn("enum class WireMode", source_h)
        self.assertNotIn("Nv21", phone_h)
        self.assertIn("feed/src/tcp_h264_feed.cpp", android_cmake)
        self.assertIn("tcp_nv21", feed_main)  # upstream compatibility entry
        self.assertNotIn("Uncompressed", ui)
        self.assertTrue((CAM / "android/feed/src/tcp_nv21_feed.cpp").exists())
        self.assertTrue((CAM / "android/feed/src/tcp_h264_feed_v2.cpp").exists())

    def test_native_core_is_present_under_gpl(self):
        self.assertTrue((CAM / "LICENSE").is_file())
        self.assertIn("GNU GENERAL PUBLIC LICENSE", (ROOT / "LICENSE").read_text(encoding="utf-8"))

    def test_product_has_one_windows_identity(self):
        launcher = (ROOT / "long_virtual_camera.py").read_text(encoding="utf-8")
        cmake = self.read("host/CMakeLists.txt")
        app = self.read("host/src/app/App.cpp")
        self.assertNotIn('"--engine"', launcher)
        self.assertIn('NATIVE_NAME = "LONG-Virtual-Camera.exe"', launcher)
        self.assertIn('OUTPUT_NAME "LONG-Virtual-Camera"', cmake)
        self.assertIn('L"LONG Virtual Camera"', app)

    def test_windows_host_runs_without_unnecessary_uac_elevation(self):
        manifest = (CAM / "host/resources/CameraReplace.manifest").read_text(encoding="utf-8")
        app = self.read("host/src/app/App.cpp")
        self.assertIn('requestedExecutionLevel level="asInvoker"', manifest)
        self.assertNotIn("requireAdministrator", manifest)
        self.assertNotIn("is_process_elevated", app)
        self.assertNotIn("admin required", app)

    def test_recam_donor_ui_does_not_block_on_status_or_device_scan(self):
        app = (ROOT / "desktop" / "recam_ui" / "app.py").read_text(encoding="utf-8")
        self.assertIn("_runtime_serial", app)
        self.assertIn("_devices_found = Signal", app)
        self.assertIn("threading.Thread(target=scan, daemon=True).start()", app)
        self.assertIn("threading.Thread(target=read_status, daemon=True).start()", app)


if __name__ == "__main__":
    unittest.main()
