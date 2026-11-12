-- Semantic groups are public: colorschemes can override any group before or after loading SOS.
local M = {}
local api = vim.api
M.namespace = api.nvim_create_namespace('sos.conversation')
function M.setup()
  local groups = {
    Normal={fg='#E6EDF3',bg='#181B1B'}, Draft={fg='#E6EDF3',bg='#303A48'},
    User={fg='#81D8D0',bold=true}, Assistant={fg='#05b07d',bold=true},
    Activity={fg='#71808E',bg='#242A31'}, Detail={fg='#277be3'},
    Plan={fg='#05b07d',bold=true}, Separator={fg='#3A4655'},
    Notice={fg='#F4A38A'}, Heading={fg='#E9C46A',bold=true},
    Bold={fg='#F4A38A',bold=true}, Code={fg='#81D8D0',bg='#101215'},
    Sane={fg='#05b07d'}, Slop={fg='#F51D53'},
  }
  local terminal = {
    ['#E6EDF3']=255, ['#181B1B']=234, ['#303A48']=237, ['#81D8D0']=116,
    ['#05b07d']=35, ['#71808E']=66, ['#242A31']=235, ['#277be3']=33,
    ['#3A4655']=239, ['#F4A38A']=216, ['#E9C46A']=222, ['#101215']=233,
    ['#F51D53']=197,
  }
  for name, value in pairs(groups) do
    value.ctermfg=terminal[value.fg]
    value.ctermbg=terminal[value.bg]
    api.nvim_set_hl(0,'Sos'..name,vim.tbl_extend('force',value,{default=true}))
  end
end
function M.apply(buffer, projection)
  api.nvim_buf_clear_namespace(buffer,M.namespace,0,-1)
  for _, mark in ipairs(projection.highlights or {}) do
    api.nvim_buf_set_extmark(buffer,M.namespace,mark.line-1,mark.first or 0,{
      end_col=mark.last or #projection.lines[mark.line],hl_group=mark.group,
      hl_eol=mark.eol or false,priority=110,
    })
  end
end
return M
