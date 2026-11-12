local M = {}
local commands = {'/help','/new','/resume','/mode','/cancel','/model','/reasoning','/permissions',
  '/context','/plan','/auto_plan','/start','/retry_plan','/implementation_plan','/generate_plan','/steps','/continue',
  '/inspect','/review','/indicator','/validate','/proposal','/accept_proposal','/reject_proposal','/revision','/versions'}

function M.complete(findstart, base)
  if findstart == 1 then
    local line = vim.api.nvim_get_current_line()
    return line:sub(1, vim.fn.col('.') - 1):match('^/[%w_]*$') and 0 or -3
  end
  return vim.tbl_filter(function(command) return command:sub(1,#base) == base end, commands)
end

function M.model(session)
  session:run(function()
    session:request({type='model.list'}, 'model.listed', function(event)
      if not event then return end
      if event.errors and #event.errors > 0 then session:notice(vim.inspect(event.errors)) end
      local models = event.models or {}
      if #models == 0 then
        session:notice('No authenticated models. Configure a provider using SOS /connect in the TUI, then reconnect. Do not enter keys in chat.')
        return
      end
      vim.ui.select(models, {prompt='SOS model',format_item=function(row)
        return tostring(row.provider) .. ' / ' .. tostring(row.id or row.model)
      end}, function(row)
        if not row then return end
        local maximum = type(row.max_context) == 'number' and row.max_context or nil
        vim.ui.input({prompt='Context budget (minimum 32768): ',default=maximum and tostring(maximum) or ''}, function(value)
          if not value then return end
          local budget = tonumber(value)
          if not budget or budget % 1 ~= 0 or budget < 32768 or (maximum and budget > maximum) then
            session:notice('Invalid context budget; model unchanged'); return
          end
          session:request({type='model.select',provider=row.provider,model=row.id or row.model,context_limit=budget}, 'model.selected')
        end)
      end)
    end)
  end)
end

function M.permission(session)
  if session.permission_prompt then return end
  local id, request = next(session.permissions)
  if not id then return end
  session.permission_prompt = id
  session:notice('Permission required: ' .. request.tool .. '\n' .. vim.inspect({
    permission=request.permission,patterns=request.patterns,always=request.always,
    details=request.details,arguments=request.arguments,working_directory=request.working_directory,
  }))
  local choices = {{label='Deny and cancel execution',decision='no'},{label='Allow once',decision='yes'}}
  if request.always and #request.always > 0 then
    table.insert(choices,{label='Always allow displayed scopes for this connection',decision='always'})
  end
  local current = session.current
  vim.ui.select(choices, {prompt='SOS permission: ' .. request.tool .. ' (Enter confirms the selected decision)',format_item=function(row) return row.label end}, function(row)
    if session.permissions[id] ~= request then return end
    session.permission_prompt = nil
    if not row then
      session:notice('Permission still pending for '..request.tool..'. Use /permissions to reopen; no permission was granted.')
      return
    end
    if session.permissions[id].pending then return end
    session.permissions[id].pending = true
    current.permission_decision = row.decision == 'no' and ('Permission denied for '..request.tool..'.') or nil
    local sent = session:request({type='permission.reply',permissionId=id,decision=row.decision}, 'permission.reply_accepted', function(event)
      if not event and session.permissions[id] then session.permissions[id].pending = false end
    end)
    if not sent and session.permissions[id] then session.permissions[id].pending = false end
  end)
end

function M.slash(session)
  local draft = session.current.draft
  local command = draft:match('^(/[%w_]+)%s*$')
  local supported = vim.tbl_contains(commands, command)
  if not supported then
    session:notice('Unsupported slash command. Use /help. Provider setup uses /connect in the SOS TUI; never enter secrets here.')
    return
  end
  -- Consume only the command; later typed text remains an independent draft.
  session:set_draft('')
  if command == '/help' then
    session:notice('Supported: ' .. table.concat(commands, ', ') .. '. Use :SosSend explicitly; Insert mode edits, Normal mode reads.')
  elseif command == '/model' then M.model(session)
  elseif command == '/reasoning' then
    session:run(function() session:request({type='reasoning.next'}, 'reasoning.updated') end)
  elseif command == '/permissions' then M.permission(session)
  else
    local action = ({['/new']='new_session',['/resume']='resume',['/mode']='toggle_mode',['/cancel']='cancel',
      ['/context']='contexts',['/plan']='plan',['/auto_plan']='auto_plan',['/start']='start',['/retry_plan']='retry_plan',
      ['/implementation_plan']='implementation_plan',['/generate_plan']='generate_plan',['/steps']='steps',['/continue']='continue',
      ['/inspect']='inspect',['/review']='review_question',['/indicator']='indicator',['/validate']='validate',
      ['/proposal']='proposal',['/accept_proposal']='accept_proposal',['/reject_proposal']='reject_proposal',
      ['/revision']='revision_plan',['/versions']='versions'})[command]
    session[action](session)
  end
  session:changed()
end

return M
