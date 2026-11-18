-- Live step choices retain the originating editor window until explicit selection.
local M={}
M.__index=M
local api=vim.api
function M.open(title,rows,selected,origin)
  local popup=require('sos.popup').new(title,{height=math.max(1,#rows),origin=origin,preview=true,
    help='Enter Select · Esc Close · Ctrl-D/U Preview'})
  local self=setmetatable({rows={},origin=popup.origin,popup=popup,buffer=popup.buffer,window=popup.window},M)
  self.namespace=api.nvim_create_namespace('sos-selector')
  vim.wo[self.window].cursorline=true
  popup.on_cancel=function(restore) self:close(restore) end
  for _,key in ipairs({'q','<Esc>','<C-c>'}) do vim.keymap.set('n',key,function() self:close() end,{buffer=self.buffer}) end
  vim.keymap.set('n','<CR>',function()
    local row=self.rows[api.nvim_win_get_cursor(self.window)[1]]
    if not row or not row.choose then return end
    self:close()
    row.choose()
  end,{buffer=self.buffer})
  api.nvim_create_autocmd('CursorMoved',{group=popup.group,buffer=self.buffer,callback=function() self:preview() end})
  for _,key in ipairs({'<C-d>','<C-u>'}) do
    vim.keymap.set('n',key,function()
      api.nvim_win_call(popup.preview_window,function()
        vim.cmd('normal! '..api.nvim_replace_termcodes(key,true,false,true))
      end)
    end,{buffer=self.buffer,silent=true})
  end
  self:update(rows,selected)
  return self
end
function M:update(rows,selected)
  if not self:visible() then return end
  local cursor=api.nvim_win_get_cursor(self.window)[1]
  local key=selected or (self.rows[cursor] or {}).key
  local lines={}
  for _,row in ipairs(rows) do lines[#lines+1]=row.label:gsub('[\r\n]',' ') end
  if #lines==0 then lines={'Loading implementation...'} end
  if not vim.deep_equal(api.nvim_buf_get_lines(self.buffer,0,-1,false),lines) then
    vim.bo[self.buffer].readonly=false
    vim.bo[self.buffer].modifiable=true
    api.nvim_buf_set_lines(self.buffer,0,-1,false,lines)
    vim.bo[self.buffer].modifiable=false
    vim.bo[self.buffer].readonly=true
  end
  api.nvim_buf_clear_namespace(self.buffer,self.namespace,0,-1)
  for n,row in ipairs(rows) do
    if row.group then api.nvim_buf_set_extmark(self.buffer,self.namespace,n-1,0,{end_col=#lines[n],hl_group=row.group}) end
    if row.key==key then cursor=n end
  end
  self.rows=rows
  self.popup.height=math.max(1,#rows)
  self.popup:layout()
  local target=math.max(1,math.min(cursor,#lines))
  if api.nvim_win_get_cursor(self.window)[1]~=target then api.nvim_win_set_cursor(self.window,{target,0}) end
  self:preview()
end
function M:preview()
  if not self:visible() then return end
  local row=self.rows[api.nvim_win_get_cursor(self.window)[1]]
  local buffer=self.popup.preview_buffer
  local window=self.popup.preview_window
  local same=self.preview_key==(row or {}).key
  local view=same and api.nvim_win_call(window,vim.fn.winsaveview)
  self.popup.title=row and row.title or 'Implementation steps'
  self.popup:layout()
  local projected=require('sos.markdown').project(table.concat(row and row.preview or {'Select a step to preview its substeps.'},'\n'))
  local lines={}
  for _,line in ipairs(projected) do lines[#lines+1]=line.text end
  if not vim.deep_equal(api.nvim_buf_get_lines(buffer,0,-1,false),lines) then
    vim.bo[buffer].modifiable=true
    api.nvim_buf_set_lines(buffer,0,-1,false,lines)
    vim.bo[buffer].modifiable=false
    vim.bo[buffer].readonly=true
  end
  api.nvim_buf_clear_namespace(buffer,self.namespace,0,-1)
  for n,line in ipairs(projected) do
    if line.group then api.nvim_buf_set_extmark(buffer,self.namespace,n-1,0,{end_col=#line.text,hl_group=line.group}) end
    for _,mark in ipairs(line.marks) do
      api.nvim_buf_set_extmark(buffer,self.namespace,n-1,mark.first,{end_col=mark.last,hl_group=mark.group,priority=110})
    end
  end
  if view then api.nvim_win_call(window,function() vim.fn.winrestview(view) end)
  else
    api.nvim_win_set_cursor(window,{1,0})
    api.nvim_win_call(window,function() vim.cmd('normal! zt') end)
  end
  self.preview_key=(row or {}).key
end
function M:visible()
  return self.window and api.nvim_win_is_valid(self.window) and api.nvim_win_get_buf(self.window)==self.buffer
end
function M:close(restore)
  self.popup:close(restore)
  self.popup:restore_mode()
  self.window=nil
end
return M
