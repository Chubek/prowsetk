-- Resolve chapter links in the combined manual. Keep Markdown source links
-- relative to their repository targets when HTML/LaTeX is built elsewhere.
local path = pandoc.path
local chapters = {}
local manual_directory = path.directory(PANDOC_STATE.input_files[1])

local function absolute(value)
    if path.is_absolute(value) then return path.normalize(value) end
    return path.normalize(path.join({pandoc.system.get_working_directory(), value}))
end

local function components(value)
    local result = {}
    for part in absolute(value):gmatch('[^/]+') do
        result[#result + 1] = part
    end
    return result
end

local function relative(target, directory)
    local destination, origin = components(target), components(directory)
    local shared = 0
    while destination[shared + 1] and
          destination[shared + 1] == origin[shared + 1] do
        shared = shared + 1
    end
    local result = {}
    for _ = shared + 1, #origin do result[#result + 1] = '..' end
    for i = shared + 1, #destination do result[#result + 1] = destination[i] end
    return table.concat(result, '/')
end

for _, source in ipairs(PANDOC_STATE.input_files) do
    local file = assert(io.open(source, 'r'))
    local contents = file:read('*a')
    file:close()
    for _, block in ipairs(pandoc.read(contents, 'markdown').blocks) do
        if block.t == 'Header' and block.level == 1 then
            chapters[path.filename(source)] = block.identifier
            break
        end
    end
end

function Link(link)
    local file, fragment = link.target:match('^([^#]+)(.*)$')
    if not file or file:match('^[%a][%w+.-]*:') or path.is_absolute(file) then
        return nil
    end
    local chapter = chapters[file]
    if chapter then
        link.target = fragment ~= '' and fragment or '#' .. chapter
    else
        local source = path.join({manual_directory, file})
        link.target = relative(source, path.directory(PANDOC_STATE.output_file)) .. fragment
    end
    return link
end
