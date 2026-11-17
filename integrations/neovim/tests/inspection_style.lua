-- Language styling uses user syntax/queries without ordinary editing hooks.
local child=vim.fn.jobstart({vim.v.progpath,'--embed','--headless','-u','NONE','-i','NONE'},{rpc=true})
assert(child>0)
local ok,err=xpcall(function()
  vim.rpcrequest(child,'nvim_ui_attach',100,36,{})
  vim.rpcrequest(child,'nvim_exec_lua',[=[
    local api=vim.api
    local root,runtime=...
    vim.opt.runtimepath:prepend(runtime)
    local syntax=root..'/runtime'
    vim.fn.mkdir(syntax..'/syntax','p')
    vim.fn.writefile({'syntax match SosUserPython /value/','let b:current_syntax = "python"'},syntax..'/syntax/python.vim')
    vim.opt.runtimepath:prepend(syntax)
    vim.cmd('syntax enable')
    api.nvim_set_hl(0,'SosUserPython',{fg=0x123456})
    api.nvim_set_hl(0,'@number.lua',{fg=0x654321})
    local hooks=0
    api.nvim_create_autocmd('FileType',{pattern={'python','sos_test_lua'},callback=function() hooks=hooks+1 end})
    api.nvim_create_autocmd('BufRead',{pattern={'*.py','*.soslua'},callback=function() hooks=hooks+1 end})
    vim.filetype.add({extension={soslua='sos_test_lua'}})
    vim.treesitter.language.register('lua','sos_test_lua')
    -- Exercise the user's per-language Tree-sitter disable and parser mapping.
    package.loaded['nvim-treesitter.configs']={
      is_enabled=function(feature,language,buffer)
        assert(feature=='highlight' and vim.bo[buffer].filetype=='sos_inspection')
        return language~='python'
      end,
      get_module=function() return {additional_vim_regex_highlighting=false} end,
    }
    local py={path='example.py',before='value = 0\n',after='value = 42\n',patch='@@ -1 +1 @@\n-value = 0\n+value = 42',
      hunk_explanations={'Update the Python value'}}
    local lua={path='example.soslua',before='return 0\n',after='return 42\n',patch='@@ -1 +1 @@\n-return 0\n+return 42',
      hunk_explanations={'Update the Lua value'}}
    vim.fn.writefile({'value = 42'},root..'/example.py')
    local client={transport={state='ready'},conversation={},sent={}}
    function client:request(frame) self.sent[#self.sent+1]=frame; return tostring(#self.sent) end
    local s=require('sos.session').new(client,{workspace=root,progress=false})
    local w=s.workflow
    w.branch_id='styling'; w.selected_identity='style'; w.info={execution_workspace=root}
    w.state={status='waiting',plan={steps={{title='Language colors',substeps={{title='Python'},{title='Lua'},{title='Empty'}}}}},
      steps={{identity='style',substeps={{complete=true,diffs={py}},{complete=true,diffs={lua}},{complete=true,diffs={}}},whole={complete=true,diffs={}}}}}
    s:inspect()
    local inspect=s.inspector
    local buffer=inspect.code_buffer
    local function plain()
      assert(vim.bo[buffer].filetype=='sos_inspection' and vim.bo[buffer].buftype=='nofile')
      assert(vim.fn.fnamemodify(api.nvim_buf_get_name(buffer),':t')=='sos-inspection')
      assert(vim.bo[buffer].readonly and not vim.bo[buffer].modifiable and hooks==0)
      for _,buf in ipairs(api.nvim_list_bufs()) do assert(api.nvim_buf_get_name(buf)~=root..'/example.py') end
    end
    plain()
    assert(inspect.source.kind=='live' and vim.bo[buffer].syntax=='python')
    local group=api.nvim_buf_call(buffer,function() return vim.fn.synIDattr(vim.fn.synID(1,1,1),'name') end)
    assert(group=='SosUserPython','Inspection did not load the user runtime syntax: '..group)
    assert(api.nvim_get_hl(0,{name='SosUserPython'}).fg==0x123456)
    s:next_substep()
    plain()
    assert(vim.treesitter.highlighter.active[buffer],'The installed Lua parser was not used')
    assert(vim.bo[buffer].syntax=='','Regex highlighting overrode the Tree-sitter preference')
    vim.treesitter.get_parser(buffer,'lua'):parse()
    local captures=vim.treesitter.get_captures_at_pos(buffer,0,7)
    assert(vim.tbl_contains(vim.tbl_map(function(item) return item.capture end,captures),'number'),
      'Inspection did not use the runtime Tree-sitter highlight query: '..vim.inspect(captures))
    assert(api.nvim_get_hl(0,{name='@number.lua'}).fg==0x654321,'Inspection replaced the user language highlight')
    s:next_substep()
    plain()
    assert(not vim.treesitter.highlighter.active[buffer] and vim.bo[buffer].syntax=='','Empty results kept the previous language')
    s:previous_substep(); s:previous_substep()
    plain()
    assert(vim.bo[buffer].syntax=='python' and not vim.treesitter.highlighter.active[buffer])
    local header=inspect.header
    local replacement=api.nvim_create_buf(true,false)
    api.nvim_buf_set_lines(replacement,0,-1,false,{'Keep this repurposed header'})
    api.nvim_win_set_buf(header,replacement)
    local height=api.nvim_win_get_height(header)
    s:next_change()
    assert(api.nvim_win_get_buf(header)==replacement and api.nvim_win_get_height(header)==height)
    inspect:hide()
    assert(api.nvim_win_is_valid(header) and api.nvim_win_get_buf(header)==replacement)
    assert(not api.nvim_buf_is_valid(buffer))
    s:inspect()
    assert(vim.fn.fnamemodify(api.nvim_buf_get_name(inspect.code_buffer),':t')=='sos-inspection')
    assert(hooks==0 and #client.sent==0)
    inspect:hide()
  ]=],{vim.fn.getcwd()..'/build-ai/neovim/inspection-style',vim.env.SOS_TEST_RUNTIME or vim.fn.getcwd()..'/integrations/neovim'})
end,debug.traceback)
pcall(vim.rpcrequest,child,'nvim_command','qa!')
pcall(vim.fn.jobwait,{child},2000)
assert(ok,err)
print('User Vim syntax, Tree-sitter queries/colors, dedicated inspection filetype and editing-hook isolation passed')
