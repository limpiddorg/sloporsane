local M = {}
local uv=vim.uv
local function inside(path,root) return path==root or path:sub(1,#root+1)==root..'/' end

function M.path(root,path)
  if type(root)~='string' or root=='' or type(path)~='string' or path=='' or path:find('\0',1,true) then return end
  local base=uv.fs_realpath(root)
  if not base then return end
  local candidate=vim.fs.normalize(path:sub(1,1)=='/' and path or base..'/'..path)
  if not inside(candidate,base) then return end
  local real=uv.fs_realpath(candidate)
  if real and inside(real,base) then return real end
end

function M.occupied(path,owned)
  for _,buffer in ipairs(vim.api.nvim_list_bufs()) do
    local name=vim.api.nvim_buf_get_name(buffer)
    if not (owned and owned[buffer]) and name~='' and (name==path or uv.fs_realpath(name)==path) then return true end
  end
  return false
end

function M.matches(path,expected)
  local stat=uv.fs_stat(path)
  if not stat or stat.type~='file' or stat.size~=#expected then return false end
  local fd=uv.fs_open(path,'r',0)
  if not fd then return false end
  local bytes=uv.fs_read(fd,#expected,0)
  uv.fs_close(fd)
  return bytes==expected
end

function M.resolve(diff,root,archive,owned)
  local patch=diff.patch or ''
  -- Metadata is recognized only before the first hunk, never inside source text.
  local header=patch:match('^(.-)\n@@ ') or (patch:match('^@@ ') and '') or patch
  local function header_line(pattern) return ('\n'..header..'\n'):find('\n'..pattern..'\n') end
  local binary=header_line('GIT binary patch') or header_line('Binary files .- differ')
    or header_line('Binary contents changed %(preserved in snapshot blobs%).')
    or (diff.after or ''):find('\0',1,true) or (diff.before or ''):find('\0',1,true)
  local deleted=header_line('%+%+%+ /dev/null') or header_line('deleted file mode %d+')
  local added=header_line('%-%-%- /dev/null') or header_line('new file mode %d+')
  if binary then return {kind='binary',label='Historical · Binary change · '..diff.path,
    text='Binary change. Contents are preserved by the daemon and are not available as text.\n'..patch} end
  if deleted then return {kind='deleted',side='before',label='Historical · Deleted · before contents · '..diff.path,text=diff.before or ''} end
  local expected=diff.after or ''
  if expected=='' and (diff.before or '')~='' then
    return {kind='empty_after',side='before',label='Historical · Empty after image (deleted or emptied) · before contents · '..diff.path,
      text=diff.before}
  end
  local path=M.path(root,diff.path)
  local opaque_kind=header:find('File kind/mode:',1,true) or header:find('120000',1,true)
  local reason=archive and 'Archive' or opaque_kind and 'File kind/mode change' or not path and 'File absent or outside isolated workspace'
    or M.occupied(path,owned) and 'Existing editor buffer' or not M.matches(path,expected) and 'Current file differs from preserved result' or nil
  if not reason then
    return {kind='live',path=path,label='Live verified · Isolated · '..diff.path,text=expected,added=added~=nil}
  end
  return {kind='historical',label='Historical · '..reason..' · '..diff.path,text=expected,added=added~=nil}
end
return M
