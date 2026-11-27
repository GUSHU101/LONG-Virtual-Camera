-- OBS Lua script for Windows x64. Uses the user-selected MediaMTX binary.
local obs = obslua
local bit = require("bit")
local ffi = require("ffi")
local supported = ffi.os == "Windows" and ffi.arch == "x64"
local win, tcp

if supported then
    ffi.cdef[[
        typedef struct {
            uint32_t cb;
            wchar_t *reserved, *desktop, *title;
            uint32_t x, y, width, height, charsX, charsY, fill, flags;
            uint16_t show, reservedSize;
            void *reserved2, *stdIn, *stdOut, *stdErr;
            void *attributes;
        } LongVcStartupInfoEx;
        typedef struct {
            void *process, *thread;
            uint32_t pid, tid;
        } LongVcProcessInfo;
        typedef struct {
            int64_t processTime, jobTime;
            uint32_t flags;
            size_t minWorkingSet, maxWorkingSet;
            uint32_t activeProcessLimit;
            uintptr_t affinity;
            uint32_t priorityClass, schedulingClass;
            uint64_t ioCounters[6];
            size_t processMemory, jobMemory, peakProcessMemory, peakJobMemory;
        } LongVcJobLimits;
        typedef struct {
            uint32_t state, localAddress, localPort, remoteAddress, remotePort, pid;
        } LongVcTcpRow;
        typedef struct {
            uint32_t address, interfaceIndex, mask, broadcast, reassembly;
            uint16_t unused, addressType;
        } LongVcIpv4Row;

        int MultiByteToWideChar(uint32_t, uint32_t, const char *, int, wchar_t *, int);
        uint32_t GetWindowsDirectoryW(wchar_t *, uint32_t);
        uint32_t GetFileAttributesW(const wchar_t *);
        void *CreateJobObjectW(void *, const wchar_t *);
        int SetInformationJobObject(void *, int, void *, uint32_t);
        int InitializeProcThreadAttributeList(void *, uint32_t, uint32_t, size_t *);
        int UpdateProcThreadAttribute(void *, uint32_t, uintptr_t, void *, size_t, void *, size_t *);
        void DeleteProcThreadAttributeList(void *);
        int CreateProcessW(const wchar_t *, wchar_t *, void *, void *, int, uint32_t,
                           void *, const wchar_t *, LongVcStartupInfoEx *, LongVcProcessInfo *);
        int CloseHandle(void *);
        uint32_t GetLastError(void);
        uint64_t GetTickCount64(void);
        uint32_t WaitForSingleObject(void *, uint32_t);
        int GetExitCodeProcess(void *, uint32_t *);
        uint32_t GetExtendedTcpTable(void *, uint32_t *, int, uint32_t, int, uint32_t);
        uint32_t GetIpAddrTable(void *, uint32_t *, int);
    ]]
    win = ffi.load("kernel32")
    tcp = ffi.load("iphlpapi")
end

local DEFAULT_PORT = 1935
local DEFAULT_PATH = "longvc"
local LOOPBACK_IP = "127.0.0.1"
local LOOPBACK_ADDRESS = 0x0100007f
local TIMER_MS = 250
local STARTUP_MS = 7000
local HEALTHY_MS = 30000
local PORT_RECHECK_MS = 2000
local MAX_RESTARTS = 3
local KILL_ON_JOB_CLOSE = 0x2000
local PROC_THREAD_ATTRIBUTE_JOB_LIST = 0x0002000d
local CREATE_HIDDEN_WITH_ATTRIBUTES = 0x08080400 -- NO_WINDOW | STARTUPINFOEX | UNICODE_ENVIRONMENT

local settings_now
local desired
local child
local retry_at = 0
local retries = 0
local waiting_for_port = false

local function info(message)
    obs.script_log(obs.LOG_INFO, message)
end

local function warn(message)
    obs.script_log(obs.LOG_WARNING, message)
end

local function win_error(operation)
    return operation .. "（Windows 错误 " .. tonumber(win.GetLastError()) .. "）"
end

local function wide(text)
    if #text == 0 or text:find("\0", 1, true) then return nil end
    local length = win.MultiByteToWideChar(65001, 8, text, #text, nil, 0)
    if length == 0 then return nil end
    local buffer = ffi.new("wchar_t[?]", length + 1)
    if win.MultiByteToWideChar(65001, 8, text, #text, buffer, length) == 0 then return nil end
    return buffer, length
end

local function file_exists(path)
    local attrs = tonumber(win.GetFileAttributesW(path))
    return attrs ~= 0xffffffff and bit.band(attrs, 0x10) == 0
end

local function read_settings(data)
    return {
        enabled = obs.obs_data_get_bool(data, "auto_start"),
        follow_obs = obs.obs_data_get_bool(data, "follow_obs"),
        exe = obs.obs_data_get_string(data, "mediamtx_exe"),
        port = obs.obs_data_get_int(data, "port"),
        path = obs.obs_data_get_string(data, "stream_path"),
        phone = obs.obs_data_get_string(data, "phone_ip"):match("^%s*(.-)%s*$"),
    }
end

local function valid_path(path)
    return #path <= 128 and path:match("^[a-zA-Z0-9_/%-]+$") and path:sub(1, 1) ~= "/"
        and path:sub(-1) ~= "/" and not path:find("//", 1, true)
end

local function valid_port(port)
    return port >= 1024 and port <= 65535
end

local function private_ipv4(address)
    local a, b, c, d = address:match("^(%d+)%.(%d+)%.(%d+)%.(%d+)$")
    if not a then return nil end
    for _, octet in ipairs({a, b, c, d}) do
        if #octet > 3 or (#octet > 1 and octet:sub(1, 1) == "0") or tonumber(octet) > 255 then
            return nil
        end
    end
    a, b, c, d = tonumber(a), tonumber(b), tonumber(c), tonumber(d)
    if a == 10 or (a == 172 and b >= 16 and b <= 31) or (a == 192 and b == 168) then
        return a + b * 256 + c * 65536 + d * 16777216
    end
    return nil
end

local function table_contains(fetch, row_type, predicate)
    local size = ffi.new("uint32_t[1]")
    local result = tonumber(fetch(nil, size))
    for _ = 1, 3 do
        if result ~= 122 or size[0] < 4 or size[0] > 8 * 1024 * 1024 then return nil end
        local table_bytes = ffi.new("uint32_t[?]", math.ceil(size[0] / 4))
        result = tonumber(fetch(table_bytes, size))
        if result == 0 then
            local count = tonumber(table_bytes[0])
            if 4 + count * ffi.sizeof(row_type) > size[0] then return nil end
            local rows = ffi.cast("const " .. row_type .. " *", table_bytes + 1)
            for i = 0, count - 1 do
                if predicate(rows[i]) then return true end
            end
            return false
        end
    end
    return nil
end

local function adapter_address(address)
    local value = private_ipv4(address)
    if not value then return nil end
    local found = table_contains(function(buffer, size)
        return tcp.GetIpAddrTable(buffer, size, 0)
    end, "LongVcIpv4Row", function(row)
        -- Ignore interfaces being removed, disconnected or used transiently.
        return row.address == value and bit.band(row.addressType, 0xc8) == 0
    end)
    return found and value or nil
end

local function obs_endpoint()
    -- This is a borrowed service. Its settings are a separate reference.
    local service = obs.obs_frontend_get_streaming_service()
    if not service then return nil, "OBS 的推流配置暂不可用。" end
    if obs.obs_service_get_id(service) ~= "rtmp_custom" then
        return nil, "请在 OBS 直播设置中选择「自定义」。"
    end
    local data = obs.obs_service_get_settings(service)
    if not data then return nil, "OBS 的推流配置暂不可用。" end
    local server = obs.obs_data_get_string(data, "server")
    local has_credentials = obs.obs_data_get_bool(data, "use_auth") or obs.obs_data_get_string(data, "key") ~= ""
    obs.obs_data_release(data)
    if has_credentials then return nil, "沿用 OBS 配置需要留空推流码，并关闭身份验证。" end

    local authority, path = server:match("^rtmp://([^/]+)/(.+)$")
    if not authority then return nil, "OBS 地址应使用本机 RTMP，并包含流路径。" end
    local host, port = authority:match("^([a-zA-Z0-9.]+):(%d+)$")
    host = (host or authority):lower()
    port = tonumber(port) or DEFAULT_PORT
    if not valid_port(port) or not valid_path(path) then
        return nil, "OBS 地址的端口或流路径无效。"
    end
    if host == "localhost" or host == LOOPBACK_IP then
        return {port = port, path = path, host = LOOPBACK_IP, address = LOOPBACK_ADDRESS}
    end
    local address = adapter_address(host)
    if not address then
        return nil, "沿用 OBS 配置只接受 localhost、127.0.0.1 或本机网卡的局域网 IPv4。"
    end
    return {port = port, path = path, host = host, address = address}
end

local function prepare(spec)
    local endpoint = {
        port = spec.port,
        path = spec.path == "" and DEFAULT_PATH or spec.path,
        host = LOOPBACK_IP,
        address = LOOPBACK_ADDRESS,
    }
    if spec.follow_obs then
        local reason
        endpoint, reason = obs_endpoint()
        if not endpoint then return nil, reason end
    end
    if not valid_port(endpoint.port) then return nil, "RTMP 端口应为 1024 至 65535。" end
    if not valid_path(endpoint.path) then
        return nil, "流路径只接受字母、数字、下划线、连字符及分隔斜杠。"
    end
    if spec.phone ~= "" and not private_ipv4(spec.phone) then
        return nil, "请填写手机的局域网 IPv4，或留空使用本机和 USB。"
    end

    local directory = script_path():gsub("/", "\\")
    if directory:sub(-1) ~= "\\" then directory = directory .. "\\" end
    local exe = spec.exe == "" and (directory .. "mediamtx.exe") or spec.exe:gsub("/", "\\")
    if not exe:match("^%a:\\") and not exe:match("^\\\\[^\\]") then
        return nil, "请选择 mediamtx.exe 的绝对路径。"
    end
    if exe:lower():match("[^\\]+$") ~= "mediamtx.exe" or exe:find('"', 1, true) then
        return nil, "请选择 MediaMTX 的 mediamtx.exe。"
    end
    local config = directory .. "mediamtx-obs.yml"
    local exe_w, config_w = wide(exe), wide(config)
    if not exe_w or not file_exists(exe_w) then return nil, "找不到 mediamtx.exe，请在脚本中选择。" end
    if not config_w or not file_exists(config_w) then return nil, "请将 mediamtx-obs.yml 放到 Lua 脚本同一目录。" end
    local bind_ip, bind_address = endpoint.host, endpoint.address
    if endpoint.host == LOOPBACK_IP and spec.phone ~= "" then
        bind_ip, bind_address = "0.0.0.0", 0
    end
    return {
        exe = exe_w,
        command = '"' .. exe .. '" "' .. config .. '"',
        port = endpoint.port,
        path = endpoint.path,
        publisher_ip = endpoint.host,
        bind_ip = bind_ip,
        bind_address = bind_address,
        phone = spec.phone,
    }
end

local function environment(spec)
    local root = ffi.new("wchar_t[32768]")
    local root_length = tonumber(win.GetWindowsDirectoryW(root, 32768))
    if root_length == 0 or root_length >= 32768 then return nil end

    local allowed = spec.publisher_ip
    if spec.phone ~= "" and spec.phone ~= allowed then allowed = allowed .. "," .. spec.phone end
    local fields = {
        "MTX_AUTHINTERNALUSERS_0_IPS=" .. spec.publisher_ip,
        "MTX_AUTHINTERNALUSERS_0_PERMISSIONS_0_PATH=" .. spec.path,
        "MTX_AUTHINTERNALUSERS_1_IPS=" .. allowed,
        "MTX_AUTHINTERNALUSERS_1_PERMISSIONS_0_PATH=" .. spec.path,
        "MTX_RTMPADDRESS=" .. spec.bind_ip .. ":" .. spec.port,
        "SystemRoot=",
    }
    -- Supply a private environment so inherited MTX_* settings cannot change
    -- the protocol listeners, access rules, recording or hook commands.
    local length = root_length + 2
    for index, field in ipairs(fields) do
        length = length + #field + (index == #fields and 0 or 1)
    end
    local block = ffi.new("wchar_t[?]", length)
    local cursor = 0
    for index, field in ipairs(fields) do
        for i = 1, #field do
            block[cursor] = field:byte(i)
            cursor = cursor + 1
        end
        if index < #fields then cursor = cursor + 1 end
    end
    ffi.copy(block + cursor, root, root_length * ffi.sizeof("wchar_t"))
    return block
end

-- If owner is nil, detect any IPv4 listener on this port. Otherwise verify
-- that MediaMTX, rather than a pre-existing listener, opened the port.
local function listening(port, owner, address)
    local port_be = (port % 256) * 256 + math.floor(port / 256)
    return table_contains(function(buffer, size)
        return tcp.GetExtendedTcpTable(buffer, size, 0, 2, 3, 0)
    end, "LongVcTcpRow", function(row)
        return row.state == 2 and bit.band(row.localPort, 0xffff) == port_be
            and (not owner or (row.pid == owner and row.localAddress == address))
    end)
end

local function start(spec)
    local in_use = listening(spec.port)
    if in_use == nil then return nil, "无法检查本机 RTMP 端口。" end
    if in_use then return nil, "TCP " .. spec.port .. " 已被占用，等待端口释放。", true end
    local env = environment(spec)
    if not env then return nil, "无法生成 MediaMTX 进程环境。" end
    local command = wide(spec.command)
    if not command then return nil, "MediaMTX 路径无效。" end

    local job = win.CreateJobObjectW(nil, nil)
    if job == nil then return nil, win_error("创建中继进程组失败") end
    local limits = ffi.new("LongVcJobLimits")
    limits.flags = KILL_ON_JOB_CLOSE
    local function fail(operation)
        local message = win_error(operation)
        win.CloseHandle(job)
        return nil, message
    end
    if win.SetInformationJobObject(job, 9, limits, ffi.sizeof(limits)) == 0 then
        return fail("设置中继退出清理失败")
    end

    local bytes = ffi.new("size_t[1]")
    win.InitializeProcThreadAttributeList(nil, 1, 0, bytes)
    if bytes[0] == 0 then return fail("初始化进程设置失败") end
    local attributes = ffi.new("uintptr_t[?]", math.ceil(tonumber(bytes[0]) / ffi.sizeof("uintptr_t")))
    if win.InitializeProcThreadAttributeList(attributes, 1, 0, bytes) == 0 then
        return fail("初始化进程设置失败")
    end
    local jobs = ffi.new("void *[1]", job)
    local process = ffi.new("LongVcProcessInfo")
    local startup = ffi.new("LongVcStartupInfoEx")
    startup.cb = ffi.sizeof(startup)
    startup.attributes = attributes
    -- Assign the job at creation, including if OBS is closed during startup.
    local error_message
    if win.UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST,
        jobs, ffi.sizeof(jobs), nil, nil) == 0 then
        error_message = win_error("无法将中继绑定到 OBS 生命周期")
    elseif win.CreateProcessW(spec.exe, command, nil, nil, 0,
        CREATE_HIDDEN_WITH_ATTRIBUTES, env, nil, startup, process) == 0 then
        error_message = win_error("启动 MediaMTX 失败")
    end
    win.DeleteProcThreadAttributeList(attributes)
    if error_message then
        win.CloseHandle(job)
        return nil, error_message
    end
    win.CloseHandle(process.thread)
    return {
        process = process.process,
        job = job,
        pid = tonumber(process.pid),
        spec = spec,
        launched = tonumber(win.GetTickCount64()),
    }
end

local function stop()
    if child and child.job then
        win.CloseHandle(child.job)
        child.job = nil
    end
end

local function schedule_restart(now)
    if not desired or retries >= MAX_RESTARTS then return false end
    retries = retries + 1
    retry_at = now + 1000 * 2 ^ (retries - 1)
    warn("MediaMTX 正在安排重启（" .. retries .. "/" .. MAX_RESTARTS .. "）。")
    return true
end

local function tick()
    local now = tonumber(win.GetTickCount64())
    if child then
        local current = win.WaitForSingleObject(child.process, 0)
        if current == 0 or current == 0xffffffff then
            local stopping = child.job == nil
            local was_ready = child.ready_at ~= nil
            local code = ffi.new("uint32_t[1]")
            win.GetExitCodeProcess(child.process, code)
            stop()
            win.CloseHandle(child.process)
            child = nil
            if not stopping then
                if not ((was_ready or retries > 0) and schedule_restart(now)) then
                    desired = nil
                    warn("MediaMTX 已退出（代码 " .. tonumber(code[0]) .. "）。请检查端口和版本。")
                end
            end
        elseif child.job and not child.ready_at then
            if listening(child.spec.port, child.pid, child.spec.bind_address) then
                child.ready_at = now
                info("中继已就绪，正在监听 TCP " .. child.spec.port .. "。")
            elseif now - child.launched >= STARTUP_MS then
                stop()
                if retries == 0 or not schedule_restart(now) then
                    desired = nil
                    warn("MediaMTX 未在指定端口开始监听。请检查端口和版本。")
                end
            end
        elseif child.ready_at and now - child.ready_at >= HEALTHY_MS then
            retries = 0
        end
    end
    if not child and desired and now >= retry_at then
        local reason, busy
        child, reason, busy = start(desired)
        if busy then
            if not waiting_for_port then warn(reason) end
            waiting_for_port = true
            retry_at = now + PORT_RECHECK_MS
        elseif not child then
            desired = nil
            warn(reason)
        else
            waiting_for_port = false
        end
    end
end

local function apply_settings()
    local next_relay, reason = prepare(settings_now)
    if not next_relay then warn(reason); return false end
    if child and child.job then
        local active = child.spec
        if active.command == next_relay.command and active.port == next_relay.port
            and active.path == next_relay.path and active.phone == next_relay.phone
            and active.publisher_ip == next_relay.publisher_ip then
            info("中继已使用这套设置。")
            return false
        end
        if obs.obs_frontend_streaming_active() then
            warn("OBS 正在推流。请在停止推流后应用中继设置。")
            return false
        end
        if next_relay.port ~= active.port and listening(next_relay.port) ~= false then
            warn("新端口不可用，当前中继继续运行。")
            return false
        end
    end
    desired, retry_at, retries = next_relay, 0, 0
    waiting_for_port = false
    stop()
    return false
end

local function stop_relay()
    desired = nil
    stop()
    info("已停止 OBS 脚本管理的中继。")
    return false
end

local function report_status()
    if child and child.job and child.ready_at then
        local active = child.spec
        local endpoint, reason = obs_endpoint()
        local access = active.phone ~= "" and "指定手机 IPv4"
            or (active.publisher_ip == LOOPBACK_IP and "本机与 USB" or "本机网卡")
        info("中继正在监听 TCP " .. active.port .. "（" .. access .. "）。")
        if not endpoint then
            warn(reason)
        elseif endpoint.port ~= active.port or endpoint.path ~= active.path
            or endpoint.host ~= active.publisher_ip then
            warn("OBS 的直播地址、端口或流路径与运行中的中继不同。")
        end
    elseif waiting_for_port and desired then
        info("等待 TCP " .. desired.port .. " 释放。")
    elseif desired then
        info("中继正在启动或等待重试。")
    else
        info("中继已停止。")
    end

    local streaming = obs.obs_frontend_streaming_active()
    local output = obs.obs_frontend_get_streaming_output()
    local reconnecting = output and obs.obs_output_reconnecting(output)
    local stats
    if output and streaming and not reconnecting then
        stats = string.format("OBS 输出 %d×%d，累计网络丢帧 %d。",
            obs.obs_output_get_width(output), obs.obs_output_get_height(output),
            obs.obs_output_get_frames_dropped(output))
    end
    if output then obs.obs_output_release(output) end
    if reconnecting then
        info("OBS 正在重连。")
    elseif streaming then
        info("OBS 正在推流。")
        if stats then info(stats) end
    else
        info("OBS 等待开始推流。")
    end
    return false
end

local function update_endpoint_fields(props, _, data)
    local manual = not obs.obs_data_get_bool(data, "follow_obs")
    obs.obs_property_set_enabled(obs.obs_properties_get(props, "port"), manual)
    obs.obs_property_set_enabled(obs.obs_properties_get(props, "stream_path"), manual)
    return true
end

function script_defaults(data)
    obs.obs_data_set_default_bool(data, "auto_start", true)
    obs.obs_data_set_default_int(data, "port", DEFAULT_PORT)
    obs.obs_data_set_default_string(data, "stream_path", DEFAULT_PATH)
    obs.obs_data_set_default_bool(data, "follow_obs", false)
end

function script_description()
    return "LONG Virtual Camera · MediaMTX 中继<br>"
        .. "使用「沿用 OBS 本机推流地址」可以读取 OBS 的端口和流路径。<br>"
        .. "OBS 使用自定义 RTMP，推流码留空，编码选 H.264。<br>"
        .. "使用局域网时填写手机 IPv4。使用 USB 时请让 OBS 推流到 127.0.0.1。"
end

function script_properties()
    local props = obs.obs_properties_create()
    if not supported then return props end
    obs.obs_properties_add_path(props, "mediamtx_exe", "MediaMTX 程序",
        obs.OBS_PATH_FILE, "MediaMTX (mediamtx.exe)", nil)
    local follow = obs.obs_properties_add_bool(props, "follow_obs", "沿用 OBS 本机推流地址")
    obs.obs_properties_add_int(props, "port", "RTMP 端口", 1024, 65535, 1)
    obs.obs_properties_add_text(props, "stream_path", "流路径", obs.OBS_TEXT_DEFAULT)
    obs.obs_property_set_modified_callback(follow, update_endpoint_fields)
    obs.obs_properties_add_text(props, "phone_ip", "允许拉流的手机 IPv4（可选）", obs.OBS_TEXT_DEFAULT)
    obs.obs_properties_add_bool(props, "auto_start", "随 OBS 自动启动中继")
    obs.obs_properties_add_button(props, "apply", "启动 / 应用中继设置", apply_settings)
    obs.obs_properties_add_button(props, "stop", "停止中继", stop_relay)
    obs.obs_properties_add_button(props, "status", "检查中继和推流状态", report_status)
    return props
end

function script_update(data)
    if supported then settings_now = read_settings(data) end
end

function script_load(data)
    if not supported then
        warn("此 MediaMTX 管理脚本支持 Windows x64。")
        return
    end
    script_update(data)
    obs.timer_add(tick, TIMER_MS)
    if settings_now.enabled then
        apply_settings()
        tick()
    end
end

function script_unload()
    if not supported then return end
    obs.timer_remove(tick)
    desired = nil
    stop()
    if child then
        win.CloseHandle(child.process)
        child = nil
    end
end
