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
    ]]
    win = ffi.load("kernel32")
    tcp = ffi.load("iphlpapi")
end

local DEFAULT_PORT = 1935
local DEFAULT_PATH = "longvc"
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
        exe = obs.obs_data_get_string(data, "mediamtx_exe"),
        port = obs.obs_data_get_int(data, "port"),
        path = obs.obs_data_get_string(data, "stream_path"),
        phone = obs.obs_data_get_string(data, "phone_ip"):match("^%s*(.-)%s*$"),
    }
end

local function valid_lan_address(address)
    local a, b, c, d = address:match("^(%d+)%.(%d+)%.(%d+)%.(%d+)$")
    if not a then return false end
    for _, octet in ipairs({a, b, c, d}) do
        if #octet > 3 or (#octet > 1 and octet:sub(1, 1) == "0") or tonumber(octet) > 255 then
            return false
        end
    end
    a, b = tonumber(a), tonumber(b)
    return a == 10 or (a == 172 and b >= 16 and b <= 31) or (a == 192 and b == 168)
end

local function prepare(spec)
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
    if spec.port < 1024 or spec.port > 65535 then return nil, "RTMP 端口应为 1024 至 65535。" end
    local path = spec.path == "" and DEFAULT_PATH or spec.path
    if #path > 128 or not path:match("^[%w_/%-]+$") or path:sub(1, 1) == "/"
        or path:sub(-1) == "/" or path:find("//", 1, true) then
        return nil, "流路径只接受字母、数字、下划线、连字符及分隔斜杠。"
    end
    if spec.phone ~= "" and not valid_lan_address(spec.phone) then
        return nil, "请填写手机的局域网 IPv4，或留空使用本机和 USB。"
    end
    return {
        exe = exe_w,
        command = '"' .. exe .. '" "' .. config .. '"',
        port = spec.port,
        path = path,
        phone = spec.phone,
    }
end

local function environment(spec)
    local root = ffi.new("wchar_t[32768]")
    local root_length = tonumber(win.GetWindowsDirectoryW(root, 32768))
    if root_length == 0 or root_length >= 32768 then return nil end

    local allowed = spec.phone == "" and "127.0.0.1" or "127.0.0.1," .. spec.phone
    local bind = spec.phone == "" and "127.0.0.1" or "0.0.0.0"
    local fields = {
        "MTX_AUTHINTERNALUSERS_0_PERMISSIONS_0_PATH=" .. spec.path,
        "MTX_AUTHINTERNALUSERS_1_IPS=" .. allowed,
        "MTX_AUTHINTERNALUSERS_1_PERMISSIONS_0_PATH=" .. spec.path,
        "MTX_RTMPADDRESS=" .. bind .. ":" .. spec.port,
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
local function listening(port, owner, loopback)
    local size = ffi.new("uint32_t[1]")
    local result = tonumber(tcp.GetExtendedTcpTable(nil, size, 0, 2, 3, 0))
    local port_be = (port % 256) * 256 + math.floor(port / 256)
    for _ = 1, 3 do
        if result ~= 122 or size[0] < 4 or size[0] > 8 * 1024 * 1024 then return nil end
        local table_bytes = ffi.new("uint32_t[?]", math.ceil(size[0] / 4))
        result = tonumber(tcp.GetExtendedTcpTable(table_bytes, size, 0, 2, 3, 0))
        if result == 0 then
            local count = tonumber(table_bytes[0])
            if 4 + count * ffi.sizeof("LongVcTcpRow") > size[0] then return nil end
            local rows = ffi.cast("const LongVcTcpRow *", table_bytes + 1)
            for i = 0, count - 1 do
                local row = rows[i]
                if row.state == 2 and bit.band(row.localPort, 0xffff) == port_be and
                    (not owner or (row.pid == owner and row.localAddress == (loopback and 0x0100007f or 0))) then
                    return true
                end
            end
            return false
        end
    end
    return nil
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
        port = spec.port,
        launched = tonumber(win.GetTickCount64()),
    }
end

local function stop()
    if child and child.job then
        win.CloseHandle(child.job)
        child.job = nil
    end
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
                if desired and was_ready and retries < MAX_RESTARTS then
                    retries = retries + 1
                    retry_at = now + 1000 * 2 ^ (retries - 1)
                    warn("MediaMTX 已退出，正在安排重启（" .. retries .. "/" .. MAX_RESTARTS .. "）。")
                else
                    desired = nil
                    warn("MediaMTX 已退出（代码 " .. tonumber(code[0]) .. "）。请检查端口和版本。")
                end
            end
        elseif child.job and desired and not child.ready_at then
            if listening(desired.port, child.pid, desired.phone == "") then
                child.ready_at = now
                info("中继已就绪，正在监听 TCP " .. desired.port .. "。")
            elseif now - child.launched >= STARTUP_MS then
                desired = nil
                stop()
                warn("MediaMTX 未在指定端口开始监听。请检查端口和版本。")
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
    if child and child.job and next_relay.port ~= child.port and listening(next_relay.port) ~= false then
        warn("新端口不可用，当前中继继续运行。")
        return false
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

function script_defaults(data)
    obs.obs_data_set_default_bool(data, "auto_start", true)
    obs.obs_data_set_default_int(data, "port", DEFAULT_PORT)
    obs.obs_data_set_default_string(data, "stream_path", DEFAULT_PATH)
end

function script_description()
    return "LONG Virtual Camera · MediaMTX 中继<br>"
        .. "加载后由 OBS 管理本机 RTMP 中继。首次请选择 MediaMTX 程序。<br>"
        .. "OBS 自定义直播默认使用 rtmp://127.0.0.1:1935/longvc，推流码留空，编码选 H.264。<br>"
        .. "使用局域网时填写手机 IPv4。留空时仅本机和 USB 可访问。"
end

function script_properties()
    local props = obs.obs_properties_create()
    if not supported then return props end
    obs.obs_properties_add_path(props, "mediamtx_exe", "MediaMTX 程序",
        obs.OBS_PATH_FILE, "MediaMTX (mediamtx.exe)", nil)
    obs.obs_properties_add_int(props, "port", "RTMP 端口", 1024, 65535, 1)
    obs.obs_properties_add_text(props, "stream_path", "流路径", obs.OBS_TEXT_DEFAULT)
    obs.obs_properties_add_text(props, "phone_ip", "允许拉流的手机 IPv4（可选）", obs.OBS_TEXT_DEFAULT)
    obs.obs_properties_add_bool(props, "auto_start", "随 OBS 自动启动中继")
    obs.obs_properties_add_button(props, "apply", "启动 / 应用并重启中继", apply_settings)
    obs.obs_properties_add_button(props, "stop", "停止中继", stop_relay)
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
