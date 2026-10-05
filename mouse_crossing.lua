local M = {}
local worker, watcher, native, config
local enabled = true
local lastError

function M.inventory()
    local result = {}
    for _, screen in ipairs(hs.screen.allScreens()) do
        local f = screen:fullFrame()
        table.insert(result, {
            name = screen:name(), uuid = screen:getUUID(), id = screen:id(),
            frame = {x = f.x, y = f.y, w = f.w, h = f.h},
        })
    end
    return result
end

local function refresh()
    if worker then worker:stop(); worker = nil end
    lastError = nil
    local displays = M.inventory()
    local names, layout = {}, {}
    for _, display in ipairs(displays) do
        names[display.name] = (names[display.name] or 0) + 1
    end
    for _, display in ipairs(displays) do
        local physical = config.displays[display.uuid]
        if not physical and names[display.name] == 1 then
            physical = config.displays[display.name]
        end
        if not physical then
            lastError = "Missing calibration for " .. display.name .. " (" .. display.uuid .. ")"
            return false, lastError
        end
        table.insert(layout, {frame = display.frame, physical = physical})
    end
    if #layout < 2 then return true end
    local ok, result = pcall(native.start, layout)
    if not ok then lastError = tostring(result); return false, lastError end
    worker = result
    worker:setEnabled(enabled)
    return true
end

function M.stop()
    if watcher then watcher:stop(); watcher = nil end
    if worker then worker:stop(); worker = nil end
end

function M.start(options)
    M.stop()
    assert(type(options) == "table" and type(options.directory) == "string", "Provide the kit directory")
    local loaded = options.layout or assert(loadfile(options.directory .. "/layout.lua"))()
    assert(type(loaded) == "table" and type(loaded.displays) == "table", "Provide a displays table")
    config = loaded
    enabled = loaded.enabled ~= false
    local loader = assert(package.loadlib(options.directory .. "/build/portable_mouse_crossing_hid.so", "luaopen_portable_mouse_crossing_hid"))
    native = loader()
    local ok, err = refresh()
    watcher = hs.screen.watcher.new(function()
        local success, message = refresh()
        if not success then hs.printf("Mouse crossing stopped: %s", message) end
    end):start()
    return ok, err
end

function M.setEnabled(value)
    assert(type(value) == "boolean", "Use true or false")
    enabled = value
    if worker then worker:setEnabled(value) end
end

function M.status()
    local result = worker and worker:status() or {running = false}
    result.enabled = enabled
    result.error = lastError
    result.displays = M.inventory()
    return result
end

return M
