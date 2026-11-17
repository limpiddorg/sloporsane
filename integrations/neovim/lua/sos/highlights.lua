-- Semantic groups are public: colorschemes can override any group before or after loading SOS.
local M = {}
local api = vim.api
M.namespace = api.nvim_create_namespace('sos.conversation')
function M.setup()
  local groups = {
    Normal={link='Normal'}, Draft={link='Normal'},
    Popup={link='NormalFloat'}, PopupBorder={link='FloatBorder'}, PopupTitle={fg='#05b07d',bold=true},
    User={bold=true},
    Activity={fg='#71808E'}, Detail={},
    Tool={fg='#F4A38A',bold=false}, Reasoning={fg='#81D8D0'}, Read={fg='#E9C46A',bold=false},
    Skill={fg='#8AD4A1',bold=false}, Argument={fg='#81D8D0'},
    Plan={bold=true}, Separator={fg='#71808E'},
    Notice={fg='#F4A38A'}, Heading={bold=true},
    Bold={bold=true}, Code={},
    Sane={fg='#05b07d'}, Slop={fg='#F51D53'},
    ModeSane={fg='#000000',bg='#05b07d',bold=true}, ModeSlop={fg='#000000',bg='#F51D53',bold=true},
    TabActive={fg='#E6EDF3',bg='#277be3',bold=true}, Version={fg='#71808E',bold=false},
    Muted={fg='#71808E'}, Path={fg='#277be3'}, Link={underline=true},
    Emphasis={italic=true}, Quote={},
    CodeTitle={}, ObjectivePlan={fg='#71808E',bold=true}, ObjectivePlanUpdated={fg='#05b07d',bold=true}, PlanBody={link='Normal'},
    Validated={fg='#8AD4A1'}, Error={fg='#F51D53'},
    Selection={fg='#E9C46A',bg='#277be3',bold=true},
    Action={fg='#E6EDF3',bg='#277be3',bold=true}, Unnecessary={fg='#71808E',strikethrough=true}, ValidationSweep={fg='#FFFFFF'},
    Added={fg='#8AD4A1',bg='#101215'}, Removed={fg='#F4A38A',bg='#101215'},
  }
  local terminal = {
    ['#000000']=16,
    ['#FFFFFF']=15,
    ['#E6EDF3']=255, ['#181B1B']=234, ['#303A48']=237, ['#81D8D0']=116,
    ['#05b07d']=35, ['#71808E']=66, ['#242A31']=235, ['#277be3']=33,
    ['#3A4655']=239, ['#F4A38A']=216, ['#E9C46A']=222, ['#101215']=233,
    ['#F51D53']=197,
    ['#8AD4A1']=114, ['#111419']=233, ['#15251E']=22,
  }
  for name, value in pairs(groups) do
    value.ctermfg=terminal[value.fg]
    value.ctermbg=terminal[value.bg]
    api.nvim_set_hl(0,'Sos'..name,vim.tbl_extend('force',value,{default=true}))
  end
end
local function mark(buffer,projection,value)
  api.nvim_buf_set_extmark(buffer,M.namespace,value.line-1,value.first or 0,{
    end_col=value.last or #projection.lines[value.line],hl_group=value.group,
    hl_eol=value.eol or false,priority=110,hl_mode='combine',
  })
end
local function band(buffer,projection,line,group)
  api.nvim_buf_set_extmark(buffer,M.namespace,line-1,0,{
    virt_text={{'█',group}},virt_text_win_col=(projection.band_columns or {})[line] or 1,
    virt_text_repeat_linebreak=true,hl_mode='replace',priority=120,
  })
end
function M.apply(buffer, projection)
  api.nvim_buf_clear_namespace(buffer,M.namespace,0,-1)
  for _,value in ipairs(projection.highlights or {}) do mark(buffer,projection,value) end
  for line,group in pairs(projection.bands or {}) do band(buffer,projection,line,group) end
end
function M.apply_lines(buffer,projection,changes)
  for _,change in ipairs(changes) do
    local line=change.line
    api.nvim_buf_clear_namespace(buffer,M.namespace,line-1,line)
    for _,value in ipairs(projection.highlights_by_line[line] or {}) do mark(buffer,projection,value) end
    if projection.bands[line] then band(buffer,projection,line,projection.bands[line]) end
  end
end
return M
