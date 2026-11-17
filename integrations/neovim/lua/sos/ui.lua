-- SOS-local dialogs: never replace vim.ui or depend on an installed picker plugin.
local M={}
local api=vim.api
local Popup=require('sos.popup')
local queue={}
local active,context
local pump
local function clean(text) return tostring(text):gsub('[%c]',' ') end
local function open(request)
  local selection=request.items~=nil
  local popup=Popup.new(request.options.prompt or (selection and 'SOS selection' or 'SOS input'),{
    height=selection and math.max(1,math.min(#request.items,12)) or 1,
    prompt=selection,editable=not selection,origin=request.origin,
    help=selection and 'Enter Select · Esc Cancel · Up/Down Navigate' or 'Enter Submit · Esc Cancel',
  })
  active=popup
  local done=false
  local function finish(value,index,restore)
    if done then return end
    done=true
    popup:close(restore)
    active=nil
    context=popup.origin
    -- Callbacks can open the next dialog only after this frame is fully torn down.
    vim.schedule(function()
      request.callback(value,index)
      context=nil
      pump()
      if not active and restore~=false then popup:restore_mode() end
    end)
  end
  popup.on_cancel=function(restore) finish(nil,nil,restore) end
  local function map(buffer,modes,key,callback)
    vim.keymap.set(modes,key,callback,{buffer=buffer,silent=true,nowait=true})
  end
  for _,buffer in ipairs(popup.buffers) do
    for _,key in ipairs({'<Esc>','<C-c>'}) do map(buffer,{'n','i'},key,function() finish() end) end
    map(buffer,'n','q',function() finish() end)
  end
  if selection then
    local entries={}
    for index,item in ipairs(request.items) do
      entries[#entries+1]={index=index,label=clean(request.options.format_item and request.options.format_item(item) or item)}
    end
    local visible,query={},nil
    local pending=false
    local function refresh()
      if done then return end
      pending=false
      local text=table.concat(api.nvim_buf_get_lines(popup.prompt_buffer,0,-1,false),' ')
      if text==query then return end
      query=text
      visible=text=='' and entries or vim.fn.matchfuzzy(entries,text,{key='label'})
      local lines={}
      for _,entry in ipairs(visible) do lines[#lines+1]='  '..entry.label end
      if #lines==0 then lines={'  No matches'} end
      vim.bo[popup.buffer].modifiable=true
      api.nvim_buf_set_lines(popup.buffer,0,-1,false,lines)
      vim.bo[popup.buffer].modifiable=false
      api.nvim_win_set_cursor(popup.window,{1,0})
    end
    local function move(offset)
      refresh()
      local row=api.nvim_win_get_cursor(popup.window)[1]
      api.nvim_win_set_cursor(popup.window,{math.max(1,math.min(math.max(1,#visible),row+offset)),0})
    end
    local function confirm()
      refresh()
      local entry=visible[api.nvim_win_get_cursor(popup.window)[1]]
      if entry then finish(request.items[entry.index],entry.index) end
    end
    vim.wo[popup.window].cursorline=true
    api.nvim_buf_attach(popup.prompt_buffer,false,{on_lines=function()
      if pending or done then return end
      pending=true
      vim.schedule(refresh)
    end})
    for _,buffer in ipairs(popup.buffers) do
      map(buffer,{'n','i'},'<CR>',confirm)
      for key,offset in pairs({['<Down>']=1,['<C-n>']=1,['<Up>']=-1,['<C-p>']=-1}) do
        map(buffer,{'n','i'},key,function() move(offset) end)
      end
      map(buffer,'n','j',function() move(1) end)
      map(buffer,'n','k',function() move(-1) end)
      map(buffer,{'n','i'},'<PageDown>',function() move(api.nvim_win_get_height(popup.window)) end)
      map(buffer,{'n','i'},'<PageUp>',function() move(-api.nvim_win_get_height(popup.window)) end)
    end
    refresh()
    api.nvim_feedkeys(api.nvim_replace_termcodes('<Esc>A',true,false,true),'ni',true)
  else
    local value=request.options.default or ''
    api.nvim_buf_set_lines(popup.buffer,0,-1,false,vim.split(value,'\n',{plain=true}))
    api.nvim_win_set_cursor(popup.window,{api.nvim_buf_line_count(popup.buffer),#value:match('[^\n]*$')})
    map(popup.buffer,{'n','i'},'<CR>',function()
      finish(table.concat(api.nvim_buf_get_lines(popup.buffer,0,-1,false),'\n'))
    end)
    api.nvim_feedkeys(api.nvim_replace_termcodes('<Esc>A',true,false,true),'ni',true)
  end
end
pump=function()
  if active or #queue==0 then return end
  open(table.remove(queue,1))
end
local function submit(items,options,callback)
  queue[#queue+1]={items=items,options=options or {},callback=callback,
    origin=context or (active and active.origin) or Popup.origin()}
  pump()
end
function M.select(items,options,callback) submit(items,options,callback) end
function M.input(options,callback) submit(nil,options,callback) end
return M
