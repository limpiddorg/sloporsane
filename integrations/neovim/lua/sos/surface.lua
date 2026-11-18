-- Reserve native layout space while keeping chrome out of keyboard navigation.
local M={}
local api=vim.api
function M.owns(view)
  return view and not view.closed and api.nvim_win_is_valid(view.window) and api.nvim_win_get_buf(view.window)==view.buffer
end
function M.close(view)
  if not view or view.closed then return end
  local owned=M.owns(view)
  view.closed=true
  if view.group then pcall(api.nvim_del_augroup_by_id,view.group) end
  if view.float and api.nvim_win_is_valid(view.float) then api.nvim_win_close(view.float,true) end
  if owned then api.nvim_win_close(view.window,true) end
  for _,buffer in ipairs({view.buffer,view.display}) do
    if api.nvim_buf_is_valid(buffer) and #vim.fn.win_findbuf(buffer)==0 then api.nvim_buf_delete(buffer,{force=true}) end
  end
end
function M.new(anchor,placement,height)
  local view={buffer=api.nvim_create_buf(false,true),display=api.nvim_create_buf(false,true)}
  for _,buffer in ipairs({view.buffer,view.display}) do
    vim.bo[buffer].bufhidden='hide'
    vim.bo[buffer].swapfile=false
    vim.bo[buffer].modeline=false
    vim.bo[buffer].filetype='sos_chrome'
    vim.bo[buffer].modifiable=false
  end
  api.nvim_win_call(anchor,function()
    vim.cmd(placement..' '..height..'split')
    view.window=api.nvim_get_current_win()
    api.nvim_win_set_buf(view.window,view.buffer)
  end)
  for name,value in pairs({number=false,relativenumber=false,signcolumn='no',foldcolumn='0',
    winbar='',statusline=' ',statuscolumn='',winfixheight=true,wrap=false}) do vim.wo[view.window][name]=value end
  view.float=api.nvim_open_win(view.display,false,{relative='win',win=view.window,row=0,col=0,
    width=api.nvim_win_get_width(view.window),height=height,focusable=false,style='minimal',zindex=40})
  view.group=api.nvim_create_augroup('SosSurface'..view.buffer,{clear=true})
  api.nvim_create_autocmd('WinEnter',{group=view.group,callback=function()
    vim.schedule(function()
      if M.owns(view) and api.nvim_get_current_win()==view.window and api.nvim_win_is_valid(anchor) then
        api.nvim_set_current_win(anchor)
      end
    end)
  end})
  api.nvim_create_autocmd({'BufWinEnter','WinClosed'},{group=view.group,callback=function()
    vim.schedule(function() if not M.owns(view) then M.close(view) end end)
  end})
  api.nvim_create_autocmd({'WinResized','VimResized'},{group=view.group,callback=function()
    if M.owns(view) and api.nvim_win_is_valid(view.float) then
      api.nvim_win_set_config(view.float,{width=api.nvim_win_get_width(view.window),height=api.nvim_win_get_height(view.window)})
    end
  end})
  return view
end
function M.put(view,lines,group)
  if not M.owns(view) or not api.nvim_win_is_valid(view.float) then return end
  vim.bo[view.display].modifiable=true
  api.nvim_buf_set_lines(view.display,0,-1,false,lines)
  vim.bo[view.display].modifiable=false
  vim.wo[view.float].winhighlight='Normal:'..(group or 'SosNormal')..',NormalNC:'..(group or 'SosNormal')
end
return M
