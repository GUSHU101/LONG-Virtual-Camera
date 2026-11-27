local obs = obslua
local CENTER_ALIGNMENT = 0 -- OBS_ALIGN_CENTER is a C macro.

local camera_ratio = "photo"
local previous

local function log(level, message)
    obs.script_log(level, message)
end

local function preview_or_program_scene()
    if obs.obs_frontend_preview_program_mode_active() then
        return obs.obs_frontend_get_current_preview_scene()
    end
    return obs.obs_frontend_get_current_scene()
end

local function selected_item(items)
    local selected
    for _, item in ipairs(items) do
        if obs.obs_sceneitem_selected(item) then
            if selected then return nil end
            selected = item
        end
    end
    return selected
end

local function safe_area(video, ratio)
    local width, height = video.base_width, video.base_height
    local output_width, output_height = video.output_width, video.output_height
    if ratio == "canvas" then return width, height end
    local aspect = ratio == "photo" and 4 / 3 or 16 / 9
    if output_height > output_width then aspect = 1 / aspect end
    -- Map the phone's centered crop on the encoded image back to OBS canvas
    -- coordinates, including profiles where base and output sizes differ.
    local target_width = width * math.min(1, output_height * aspect / output_width)
    local target_height = height * math.min(1, output_width / (output_height * aspect))
    -- Round inward and leave one pixel on each edge for integer crop rounding.
    return math.max(2, math.floor(target_width / 2) * 2 - 2),
        math.max(2, math.floor(target_height / 2) * 2 - 2)
end

local function apply_to_item(scene_source, item, video)
    if obs.obs_sceneitem_locked(item) or obs.obs_sceneitem_is_group(item) then
        log(obs.LOG_WARNING, "请选择未锁定的单个来源。")
        return
    end
    local source = obs.obs_sceneitem_get_source(item)
    local width, height = obs.obs_source_get_width(source), obs.obs_source_get_height(source)
    if width <= 0 or height <= 0 then
        log(obs.LOG_WARNING, "来源还没有画面尺寸，请等待加载后重试。")
        return
    end

    local old = obs.obs_transform_info()
    local crop = obs.obs_sceneitem_crop()
    obs.obs_sceneitem_get_info2(item, old)
    obs.obs_sceneitem_get_crop(item, crop)
    local radians = math.rad(old.rot)
    local sin, cos = math.abs(math.sin(radians)), math.abs(math.cos(radians))
    local area_width, area_height = safe_area(video, camera_ratio)
    local scale = math.min(area_width / (width * cos + height * sin),
        area_height / (width * sin + height * cos))

    local adjusted = obs.obs_transform_info()
    obs.obs_sceneitem_get_info2(item, adjusted)
    adjusted.pos.x, adjusted.pos.y = video.base_width / 2, video.base_height / 2
    adjusted.scale.x = old.scale.x < 0 and -scale or scale
    adjusted.scale.y = old.scale.y < 0 and -scale or scale
    adjusted.alignment = CENTER_ALIGNMENT
    adjusted.bounds_type = obs.OBS_BOUNDS_NONE
    adjusted.crop_to_bounds = false

    local full = obs.obs_sceneitem_crop()
    full.left, full.top, full.right, full.bottom = 0, 0, 0, 0
    previous = {
        scene = obs.obs_source_get_uuid(scene_source),
        source = obs.obs_source_get_uuid(source),
        item_id = obs.obs_sceneitem_get_id(item),
        transform = old,
        crop = crop,
    }
    obs.obs_sceneitem_defer_update_begin(item)
    obs.obs_sceneitem_set_crop(item, full)
    obs.obs_sceneitem_set_info2(item, adjusted)
    obs.obs_sceneitem_defer_update_end(item)
    log(obs.LOG_INFO, string.format("已按原比例放入画布中央 %d×%d 区域。", area_width, area_height))
end

local function fit_source()
    if not obs.obs_sceneitem_set_info2 then
        log(obs.LOG_WARNING, "构图功能需要 OBS Studio 30.1 或更新版本。")
        return false
    end
    local video = obs.obs_video_info()
    if not obs.obs_get_video_info(video) or video.base_width < 2 or video.base_height < 2
        or video.output_width < 2 or video.output_height < 2 then
        log(obs.LOG_WARNING, "OBS 画布尚未就绪。")
        return false
    end
    local scene_source = preview_or_program_scene()
    if not scene_source then return false end
    local scene = obs.obs_scene_from_source(scene_source)
    local items = scene and obs.obs_scene_enum_items(scene)
    local item = items and selected_item(items)
    if item then
        apply_to_item(scene_source, item, video)
    else
        log(obs.LOG_WARNING, "请在 OBS 来源列表选中一个未分组的来源。")
    end
    if items then obs.sceneitem_list_release(items) end
    obs.obs_source_release(scene_source)
    return false
end

local function restore_source()
    if not previous then return false end
    local source = preview_or_program_scene()
    if not source then return false end
    local scene = obs.obs_scene_from_source(source)
    if obs.obs_source_get_uuid(source) == previous.scene and scene then
        local item = obs.obs_scene_find_sceneitem_by_id(scene, previous.item_id)
        local item_source = item and obs.obs_sceneitem_get_source(item)
        if item_source and obs.obs_source_get_uuid(item_source) == previous.source
            and not obs.obs_sceneitem_locked(item) then
            obs.obs_sceneitem_defer_update_begin(item)
            obs.obs_sceneitem_set_crop(item, previous.crop)
            obs.obs_sceneitem_set_info2(item, previous.transform)
            obs.obs_sceneitem_defer_update_end(item)
            previous = nil
            log(obs.LOG_INFO, "已恢复此前的来源构图。")
        else
            log(obs.LOG_WARNING, "原来源已锁定、移除或替换。")
        end
    else
        log(obs.LOG_WARNING, "请切回应用构图时的场景。")
    end
    obs.obs_source_release(source)
    return false
end

function script_description()
    return "完整适配手机画面<br>"
        .. "在 OBS 来源列表选一张照片或视频，选择手机拍摄比例，再点击适配。"
        .. "适配会清除该来源的场景裁剪并等比例居中。可恢复上一次构图。"
end

function script_defaults(data)
    obs.obs_data_set_default_string(data, "camera_ratio", "photo")
end

function script_update(data)
    camera_ratio = obs.obs_data_get_string(data, "camera_ratio")
end

function script_properties()
    local props = obs.obs_properties_create()
    local ratio = obs.obs_properties_add_list(props, "camera_ratio", "手机拍摄比例",
        obs.OBS_COMBO_TYPE_LIST, obs.OBS_COMBO_FORMAT_STRING)
    obs.obs_property_list_add_string(ratio, "照片 4:3 / 3:4", "photo")
    obs.obs_property_list_add_string(ratio, "录像 16:9 / 9:16", "video")
    obs.obs_property_list_add_string(ratio, "整个 OBS 画布", "canvas")
    obs.obs_properties_add_button(props, "fit", "完整适配所选来源", fit_source)
    obs.obs_properties_add_button(props, "restore", "恢复上一次构图", restore_source)
    return props
end
