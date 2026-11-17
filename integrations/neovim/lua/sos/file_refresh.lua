local api=vim.api
local M={}
M.__index=M
local terminal={['execution.completed']=true,['execution.failed']=true,['execution.cancelled']=true}
local function path(value)
  if type(value)~='string' or value:sub(1,1)~='/' or value:find('\0',1,true) then return end
  local normalized=vim.fs.normalize(value)
  return vim.uv.fs_realpath(normalized) or normalized
end
local function inside(file,root) return file==root or file:sub(1,#root+1)==root..'/' or root=='/' end

function M.new(session) return setmetatable({session=session,pending={}},M) end

function M:eligible(buffer)
  if not api.nvim_buf_is_loaded(buffer) then return false end
  local bo=vim.bo[buffer]
  -- An unset buffer-local autoread inherits the global option (Lua returns nil).
  local autoread=bo.autoread
  if autoread==nil then autoread=vim.go.autoread end
  local inspector=self.session.inspector
  return bo.buftype=='' and bo.modifiable and not bo.modified and autoread
    and not (inspector and inspector.buffers and inspector.buffers[buffer])
end

function M:queue(paths,roots)
  local targets,workspaces={},{}
  for _,value in ipairs(paths or {}) do
    local file=path(value)
    if file then targets[file]=true end
  end
  for _,value in ipairs(roots or {}) do
    local root=path(value)
    if root then workspaces[root]=true end
  end
  if not next(targets) and not next(workspaces) then return end
  for _,buffer in ipairs(api.nvim_list_bufs()) do
    if self:eligible(buffer) then
      local name=api.nvim_buf_get_name(buffer)
      local file=path(name)
      local selected=file and targets[file]
      if file and not selected then
        for root in pairs(workspaces) do if inside(file,root) then selected=true; break end end
      end
      if selected then self.pending[buffer]=name end
    end
  end
  if self.scheduled or not next(self.pending) then return end
  self.scheduled=true
  vim.schedule(function()
    self.scheduled=false
    local pending=self.pending
    self.pending={}
    for buffer,name in pairs(pending) do
      -- The user may have edited, renamed or closed the buffer since the tool result.
      -- Never check dirty buffers: even acknowledging their timestamp changes the
      -- native conflict-warning lifecycle. Respect noautoread without prompting.
      if self:eligible(buffer) and api.nvim_buf_get_name(buffer)==name then
        local stat=vim.uv.fs_stat(name)
        if stat and stat.type=='file' then pcall(vim.cmd,'checktime '..buffer) end
      end
    end
  end)
end

function M:imported(root,diffs)
  root=path(root)
  if not root then return end
  local paths={}
  for _,diff in ipairs(diffs or {}) do
    if type(diff.path)=='string' then
      local file=path(diff.path:sub(1,1)=='/' and diff.path or root..'/'..diff.path)
      if file and inside(file,root) then paths[#paths+1]=file end
    end
  end
  self:queue(paths)
end

function M:event(event)
  if event.type=='connection.closed' or event.type=='session.reset' then self.pending={}; return end
  local w=self.session.workflow
  local id=event.sessionId
  if not id or (id~=w.attached_id and id~=w.branch_id and id~=self.session.current.id) then return end
  if event.type=='harness.message_arrived' then
    local paths={}
    for _,message in ipairs(event.messages or {}) do
      if type(message.content)=='table' then
        for _,block in ipairs(message.content) do
          local metadata=block.metadata
          -- These are the native editing tools' result fields, not historical
          -- Activity summaries or paths guessed from shell commands/patch text.
          if block.type=='tool_result' and type(metadata)=='table' and type(metadata.diff)=='string' then
            paths[#paths+1]=metadata.filepath
            if type(metadata.files)=='table' then
              for _,file in ipairs(metadata.files) do
                paths[#paths+1]=file.filePath
                paths[#paths+1]=file.movePath
              end
            end
          end
        end
      end
    end
    self:queue(paths)
  elseif terminal[event.type] then
    -- Shell commands do not reliably report affected paths. Check only loaded,
    -- eligible files in this execution's workspace at its terminal event.
    local info=event.info or self.session.info or w.info
    local roots={}
    if info.workspace then roots[#roots+1]=info.workspace end
    if info.execution_workspace then roots[#roots+1]=info.execution_workspace end
    self:queue(nil,roots)
  end
end

return M
