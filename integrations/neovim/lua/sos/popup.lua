-- Shared native floating frame for SOS dialogs and the live step selector.
local M={}
M.__index=M
local api=vim.api
local function valid(win) return win and api.nvim_win_is_valid(win) end
local function clean(text) return tostring(text or ''):gsub('[%c]',' ') end
function M.origin()
  local win=api.nvim_get_current_win()
  return {window=win,buffer=api.nvim_win_get_buf(win),insert=api.nvim_get_mode().mode:sub(1,1)=='i'}
end
function M:owns(win) return win==self.window or win==self.prompt_window or win==self.preview_window end
function M:layout()
  if self.closed then return end
  local available=math.max(3,vim.o.lines-vim.o.cmdheight)
  local width=math.max(1,math.min(80,math.floor(vim.o.columns*.75),vim.o.columns-2))
  local height=math.max(1,math.min(self.height,available-(self.prompt_window and 5 or 2)))
  local total=height+(self.prompt_window and 5 or 2)
  local row=math.max(0,math.floor((available-total)/2))
  local col=math.max(0,math.floor((vim.o.columns-width-2)/2))
  local fit=require('sos.tool_format').fit
  local function config(title,y,h,x,w)
    w=w or width
    return {relative='editor',width=w,height=h,row=y,col=x or col,style='minimal',border='rounded',
      title=' '..fit(clean(title),math.max(0,w-2))..' ',title_pos='center',zindex=80}
  end
  if valid(self.preview_window) then
    local span=math.max(2,math.min(140,math.floor(vim.o.columns*.9),vim.o.columns-5))
    width=math.max(1,math.floor(span*.45))
    local right=math.max(1,span-width)
    col=math.max(0,math.floor((vim.o.columns-span-5)/2))
    height=math.max(1,math.min(math.max(self.height,math.floor(available*.65)),available-2))
    row=math.max(0,math.floor((available-height-2)/2))
    api.nvim_win_set_config(self.preview_window,config('Substeps',row,height,col+width+3,right))
  end
  if valid(self.prompt_window) then
    api.nvim_win_set_config(self.prompt_window,config(self.title,row,1))
    row=row+3
  end
  if valid(self.window) then
    local cfg=config(self.prompt_window and 'Results' or self.title,row,height)
    cfg.footer=' '..fit(self.help,math.max(0,width-2))..' '
    cfg.footer_pos='center'
    api.nvim_win_set_config(self.window,cfg)
  end
end
function M.new(title,options)
  options=options or {}
  require('sos.highlights').setup()
  local self=setmetatable({title=title,height=options.height or 1,help=options.help or 'Enter Select · Esc Close',
    origin=options.origin or M.origin(),buffers={}},M)
  local function window(editable)
    local buffer=api.nvim_create_buf(false,true)
    self.buffers[#self.buffers+1]=buffer
    vim.bo[buffer].bufhidden='wipe'
    vim.bo[buffer].swapfile=false
    vim.bo[buffer].undolevels=-1
    vim.bo[buffer].filetype=editable and 'sos_prompt' or 'sos_popup'
    local win=api.nvim_open_win(buffer,false,{relative='editor',row=0,col=0,width=1,height=1,
      style='minimal',border='rounded',zindex=80})
    vim.wo[win].winhighlight='Normal:SosPopup,NormalFloat:SosPopup,FloatBorder:SosPopupBorder,FloatTitle:SosPopupTitle,FloatFooter:SosMuted,CursorLine:SosSelection'
    vim.wo[win].wrap=false
    vim.wo[win].scrolloff=0
    vim.wo[win].sidescrolloff=0
    vim.wo[win].foldenable=false
    vim.wo[win].winbar=''
    vim.wo[win].statuscolumn=''
    return buffer,win
  end
  vim.cmd('stopinsert')
  self.buffer,self.window=window(options.editable)
  if options.prompt then self.prompt_buffer,self.prompt_window=window(true) end
  if options.preview then
    self.preview_buffer,self.preview_window=window(false)
    vim.wo[self.preview_window].wrap=true
    api.nvim_win_set_config(self.preview_window,{focusable=false})
  end
  self:layout()
  api.nvim_set_current_win(self.prompt_window or self.window)
  self.group=api.nvim_create_augroup('SosPopup'..self.buffer,{clear=true})
  api.nvim_create_autocmd('VimResized',{group=self.group,callback=function() self:layout() end})
  api.nvim_create_autocmd('WinClosed',{group=self.group,callback=function(event)
    if self:owns(tonumber(event.match)) then
      vim.schedule(function() if not self.closed then self:cancel() end end)
    end
  end})
  api.nvim_create_autocmd('BufWipeout',{group=self.group,callback=function(event)
    if vim.tbl_contains(self.buffers,event.buf) then
      vim.schedule(function() if not self.closed then self:cancel() end end)
    end
  end})
  api.nvim_create_autocmd('WinLeave',{group=self.group,callback=function()
    vim.schedule(function()
      if not self.closed and not self:owns(api.nvim_get_current_win()) then self:cancel(false) end
    end)
  end})
  return self
end
function M:cancel(restore)
  if self.on_cancel then self.on_cancel(restore) else self:close(restore); self:restore_mode() end
end
function M:close(restore)
  if self.closed then return end
  self.closed=true
  if self.group then api.nvim_del_augroup_by_id(self.group) end
  local focused=self:owns(api.nvim_get_current_win())
  if focused then vim.cmd('stopinsert') end
  for _,win in pairs({self.window,self.prompt_window,self.preview_window}) do if valid(win) then api.nvim_win_close(win,true) end end
  for _,buffer in ipairs(self.buffers) do
    if api.nvim_buf_is_valid(buffer) then api.nvim_buf_delete(buffer,{force=true}) end
  end
  if restore~=false and (focused or api.nvim_get_current_win()==self.origin.window) and valid(self.origin.window) then
    api.nvim_set_current_win(self.origin.window)
  end
end
function M:restore_mode()
  local origin=self.origin
  if valid(origin.window) and api.nvim_get_current_win()==origin.window and api.nvim_win_get_buf(origin.window)==origin.buffer then
    if origin.insert then vim.cmd('startinsert') else vim.cmd('stopinsert') end
  end
end
return M
