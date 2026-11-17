-- Real attached UI: filtering, keyboard cancellation, input, teardown and focus.
local child=vim.fn.jobstart({vim.v.progpath,'--embed','--headless','-u','NONE','-i','NONE'},{rpc=true})
assert(child>0)
local function exec(code,...) return vim.rpcrequest(child,'nvim_exec_lua',code,{...}) end
local function input(keys) vim.rpcrequest(child,'nvim_input',keys) end
local function wait(code) assert(vim.wait(2000,function() return exec(code) end,10),code) end
local ok,err=xpcall(function()
  vim.rpcrequest(child,'nvim_ui_attach',90,32,{})
  exec([[
    vim.opt.runtimepath:prepend(...)
    ui=require('sos.ui')
    origin=vim.api.nvim_get_current_win()
    origin_buffer=vim.api.nvim_get_current_buf()
    vim.api.nvim_buf_set_lines(origin_buffer,0,-1,false,{'Retained draft'})
    original_select,original_input=vim.ui.select,vim.ui.input
    function floats()
      local rows={}
      for _,win in ipairs(vim.api.nvim_list_wins()) do
        if vim.api.nvim_win_get_config(win).relative~='' then rows[#rows+1]=win end
      end
      return rows
    end
    function selected(value,index) result={value=value,index=index}; calls=(calls or 0)+1 end
    items={{id='a',label='Alpha'},{id='b',label='Beta'},{id='c',label='Gamma'}}
    ui.select(items,{prompt='Choose model',format_item=function(row) return row.label end},selected)
    assert(#floats()==2)
    for _,win in ipairs(floats()) do
      local config=vim.api.nvim_win_get_config(win)
      assert(config.relative=='editor' and config.border and config.zindex==80)
    end
    assert(vim.ui.select==original_select and vim.ui.input==original_input)
  ]],vim.fn.getcwd()..'/integrations/neovim')
  wait("return vim.api.nvim_get_mode().mode=='i'")
  input('bt')
  wait([[for _,win in ipairs(floats()) do
    local buffer=vim.api.nvim_win_get_buf(win)
    if vim.bo[buffer].filetype=='sos_popup' then
      return vim.api.nvim_buf_get_lines(buffer,0,1,false)[1]=='  Beta'
    end
  end return false]])
  input('<CR>')
  wait('return result and result.value==items[2] and result.index==2 and #floats()==0')
  exec([[assert(vim.api.nvim_get_current_win()==origin and calls==1)
    assert(vim.api.nvim_buf_get_lines(origin_buffer,0,1,false)[1]=='Retained draft')
    result=nil; ui.select(items,{prompt='Cancel'},selected)]])
  wait("return vim.api.nvim_get_mode().mode=='i'")
  input('zzzz')
  wait([[for _,win in ipairs(floats()) do
    if vim.bo[vim.api.nvim_win_get_buf(win)].filetype=='sos_popup' then
      return vim.api.nvim_buf_get_lines(vim.api.nvim_win_get_buf(win),0,1,false)[1]=='  No matches'
    end
  end return false]])
  input('<CR>')
  exec('assert(result==nil and #floats()==2)')
  input('<Esc>')
  wait('return result and result.value==nil and calls==2 and #floats()==0')
  -- Nested callbacks open their own frame after the prior buffers are gone.
  exec([[result=nil
    ui.select({'Manual','Auto'},{prompt='Implement'},function(value)
      assert(#floats()==0)
      if value then ui.input({prompt='Budget',default='32768'},selected) end
    end)]])
  wait("return vim.api.nvim_get_mode().mode=='i'")
  input('<Down><CR>')
  wait([[return #floats()==1 and vim.bo[vim.api.nvim_get_current_buf()].filetype=='sos_prompt'
    and vim.api.nvim_get_mode().mode=='i']])
  input('<C-u>65536<CR>')
  wait("return result and result.value=='65536' and #floats()==0")
  -- Restore Insert mode on cancellation, including external window closure.
  exec("vim.cmd('startinsert')")
  wait("return vim.api.nvim_get_mode().mode=='i'")
  exec([[result=nil; ui.input({prompt='Cancel input'},selected)]])
  wait("return #floats()==1 and vim.api.nvim_get_mode().mode=='i'")
  input('<Esc>')
  wait("return result and result.value==nil and #floats()==0 and vim.api.nvim_get_mode().mode=='i'")
  exec([[result=nil; ui.select({}, {prompt='Empty'}, selected)
    vim.api.nvim_win_close(vim.api.nvim_get_current_win(),true)]])
  wait('return result and result.value==nil and #floats()==0')
  -- Queued dialogs do not overwrite an unanswered choice.
  exec([[result=nil; ui.select({'First'},{prompt='First'},selected)
    ui.select({'Second'},{prompt='Second'},selected)]])
  wait("return #floats()==2 and vim.api.nvim_get_mode().mode=='i'")
  input('<CR>')
  wait("return result and result.value=='First' and #floats()==2 and vim.api.nvim_get_mode().mode=='i'")
  vim.rpcrequest(child,'nvim_ui_try_resize',30,12)
  exec([[for _,win in ipairs(floats()) do
    local cfg=vim.api.nvim_win_get_config(win)
    assert(cfg.width<=28 and cfg.height>=1)
  end]])
  input('<C-c>')
  wait('return result and result.value==nil and #floats()==0')
  exec([[assert(vim.api.nvim_get_current_win()==origin)
    for _,buffer in ipairs(vim.api.nvim_list_bufs()) do
      assert(not vim.bo[buffer].filetype:match('^sos_p'))
    end]])
end,debug.traceback)
pcall(vim.rpcrequest,child,'nvim_command','qa!')
pcall(vim.fn.jobwait,{child},2000)
assert(ok,err)
print('Floating popup filtering, input, cancellation, nesting, resize and focus tests passed')
