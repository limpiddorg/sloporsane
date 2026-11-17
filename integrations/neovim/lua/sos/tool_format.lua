-- Chat tool labels and bodies mirror turn_activity.py and tool_view.py in the TUI.
local M = {}
function M.fit(text,width)
  if not width or vim.fn.strdisplaywidth(text)<=width then return text end
  if width<=0 then return '' end
  local count=vim.fn.strchars(text)
  while count>0 and vim.fn.strdisplaywidth(vim.fn.strcharpart(text,0,count))>width-1 do count=count-1 end
  return vim.fn.strcharpart(text,0,count)..'…'
end
local function plain(value)
  if type(value)=='string' then return value end
  if value==nil or value==vim.NIL then return '' end
  return vim.json.encode(value)
end
local function duration(ms)
  local seconds=math.max(0,math.floor(ms/1000))
  if seconds<60 then return seconds..'s' end
  if seconds<3600 then return string.format('%dm %02ds',math.floor(seconds/60),seconds%60) end
  return string.format('%dh %02dm %02ds',math.floor(seconds/3600),math.floor(seconds/60)%60,seconds%60)
end
function M.generation(metadata)
  if type(metadata)=='table' and type(metadata.model)=='string' then
    local reasoning=metadata.reasoning=='none' and 'No reasoning' or metadata.reasoning or '—'
    return metadata.model..' | '..reasoning..(type(metadata.duration_ms)=='number' and ' | '..duration(metadata.duration_ms) or '')
  end
end
local function permissions(records)
  local labels={yes='Human',no='Human Refused',always='Human',ai_accepted='AI',ai_refused='AI',manually_approved='Human'}
  local displayed,seen={},{}
  for _,record in ipairs(records or {}) do
    local label=labels[record.decision]
    if label then
      if type(record.confidence)=='number' and record.confidence>=0 and record.confidence<=1 then
        local score=string.format('%.12g%%',record.confidence*100)
        label=label..(record.decision:match('^ai_') and ' '..score or ' (AI '..score..')')
      end
      local group=(record.decision=='no' or record.decision=='ai_refused') and 'SosError' or 'SosSane'
      if not seen[label..group] then displayed[#displayed+1]={label,group}; seen[label..group]=true end
    end
  end
  return displayed
end

function M.activity(record, rows)
  if record.label=='Compaction' then
    local frames={'-','\\','|','/'}
    return 'Compaction'..(record._started_at and (' '..frames[math.floor(vim.uv.hrtime()/15e7)%4+1]) or ''),{}
  end
  local kinds={}
  for _,kind in ipairs(record.kinds or {}) do kinds[kind]=true end
  for _,item in ipairs(rows or {}) do
    local kind=item.kind=='reasoning' and 'reasoning' or item.toolName=='read' and 'read'
      or (item.toolName=='skill' or item.skill_script) and 'skill' or 'tools'
    kinds[kind]=true
    for _,permission in ipairs(((item.result or {}).metadata or {}).permissions or {}) do
      if permission.decision=='ai_refused' then kinds.refused=true end
    end
  end
  local label,marks=record.label or 'Activity',{}
  for _,entry in ipairs({{'tools','SosTool'},{'reasoning','SosReasoning'},{'read','SosRead'},{'skill','SosSkill'},{'refused','SosError'}}) do
    if kinds[entry[1]] then
      local first=#label+1
      label=label..' ▪'
      marks[#marks+1]={first=first,last=#label,group=entry[2]}
    end
  end
  local count=record.count or #(rows or {})
  local ms=(record.metadata or {}).duration_ms or record.duration_ms
  if record._started_at then
    ms=math.max(0,vim.uv.hrtime()/1e6-record._started_at)
    local frames={'-','\\','|','/'}
    label=label..' '..frames[math.floor(ms/150)%#frames+1]
  end
  if count>0 then label=label..' '..count..(count==1 and ' step' or ' steps') end
  if type(ms)=='number' then label=label..(count>0 and ' | ' or ' ')..duration(ms) end
  local call=record.model_call
  if type(ms)=='number' and type(call)=='table' and type(call.output_tokens)=='number'
    and call.output_tokens>=0 and call.output_tokens<=9007199254740991 and call.output_tokens%1==0
    and type(call.duration_ms)=='number' and call.duration_ms>0 and call.duration_ms<=9007199254740991 and call.duration_ms%1==0 then
    local rate=string.format('%.1f',call.output_tokens*1000/call.duration_ms):gsub('%.0$','')
    label=label..' - '..rate..' tok/s'
  end
  return label,marks
end

function M.title(item,width)
  local args=item.arguments or {}
  if item.kind=='reasoning' then return item.title or 'Reasoning','SosReasoning',{} end
  local tool=item.toolName or 'Tool'
  local result=item.result
  local meta=(result or {}).metadata or {}
  local label=item.skill_script and (item.skill_script..' script') or tool
  if tool:match('^mcp_') then label=meta.mcp_server and meta.mcp_tool and ('mcp • '..meta.mcp_server..' • '..meta.mcp_tool) or 'mcp' end
  if item.skill_script_path then label=label..' · '..item.skill_script_path end
  if tool=='skill' and args.name then label=label..' · '..args.name end
  local group=(tool=='skill' or item.skill_script) and 'SosSkill' or tool=='read' and 'SosRead' or 'SosTool'
  local marks={}
  local function append(separator,text,hl)
    local first=#label+#separator
    label=label..separator..tostring(text):gsub('[\r\n]',' ')
    if hl then marks[#marks+1]={first=first,last=#label,group=hl} end
  end
  local authorization=permissions(meta.permissions)
  for _,entry in ipairs(authorization) do append(' • ',entry[1],entry[2]) end
  if tool=='read' then
    if result and result.isError then append(' · ','Failed','SosError') end
    if args.filePath then append(' · ',args.filePath,'SosMuted') end
    return label,group,marks
  end
  local shell=tool=='bash' or tool=='wait'
  local returned=shell and type(meta.exit)=='number' and not meta.timedOut and not meta.cancelled
  local failed=result and ((result.isError and not returned) or meta.timedOut)
  local status=not result and 'Pending' or meta.cancelled and (meta.started and 'Cancelled' or 'Not run') or failed and 'Failed' or 'Completed'
  append(#authorization>0 and ' • ' or ' · ',status,
    status=='Completed' and 'SosSane' or status=='Failed' and 'SosError' or nil)
  local hint=(shell and args.mainCommand) or ((tool=='grep' or tool=='glob') and args.pattern) or args.filePath
  if not hint and args.filePaths then hint=table.concat(args.filePaths,', ') end
  if hint then
    local tail=returned and (' · Exit code '..meta.exit) or ''
    local budget=width and math.max(0,width-vim.fn.strdisplaywidth(label..' • '..tail))
    append(' • ',M.fit(tostring(hint):gsub('[\r\n]',' '),budget),'SosMuted')
  end
  if returned then append(' · ','Exit code '..meta.exit,meta.exit==0 and 'SosSane' or 'SosError') end
  return label,group,marks
end

function M.details(item)
  local rows={}
  local function append(text,group)
    for _,line in ipairs(vim.split(plain(text),'\n',{plain=true})) do rows[#rows+1]={text=line,group=group} end
  end
  local function field(name,value)
    local text=plain(value)
    rows[#rows+1]={text=name..(text:find('\n',1,true) and ':' or ': '..text),marks={{first=0,last=#name,group='SosArgument'}}}
    if text:find('\n',1,true) then append(text) end
  end
  local function rule() rows[#rows+1]={separator=true,group='SosSeparator'} end
  if item.kind=='reasoning' then append(item.text or item.content or ''); return rows end
  local args=item.arguments or {}
  local tool=item.toolName
  local result=item.result
  local meta=(result or {}).metadata or {}
  local file_tool=tool=='write' or tool=='edit' or tool=='apply_patch'
  if tool=='grep' or tool=='glob' then
    append(args.path or '.','SosPath'); append(args.pattern or '','SosArgument')
  elseif tool=='bash' or tool=='wait' then
    field('command',args.command or '')
    field('timeout',tostring(args.timeout or (tool=='wait' and 3600000 or 120000))..' ms')
    if args.workdir then field('workdir',args.workdir) end
  elseif tool=='skill' then
    append(args.name or '','SosSkill')
    local names,values=meta.argument_names or {},meta.argument_values
    if not values then values=args.input and {args.input} or {} end
    for n=1,math.max(#names,#values) do field(names[n] or (#values==1 and 'argument' or 'argument '..(n-1)),values[n] or '') end
  elseif not file_tool then
    append('Arguments','SosRead')
    local keys=vim.tbl_keys(args); table.sort(keys)
    if #keys==0 then append('No arguments','SosMuted') end
    for _,key in ipairs(keys) do field(key,args[key]) end
  end
  if not file_tool then rule() end
  if not result then append('Waiting for result…','SosMuted'); return rows end
  local output=plain(result.output)
  if result.isError then
    local refusal
    for _,record in ipairs(meta.permissions or {}) do if record.decision=='ai_refused' then refusal=record end end
    if refusal then output=refusal.confidence==nil and 'Action not executed: AI verification unavailable.' or 'Action not executed: refused by AI verification.' end
    append(output,'SosError')
  elseif tool=='write' and type(args.content)=='string' then
    append(meta.filepath or args.filePath or 'Unknown file','SosPath'); append(args.content,'SosCode')
  elseif tool=='edit' and type(meta.diff)=='string' and meta.diff~='' then
    append(meta.filepath or args.filePath or 'Unknown file','SosPath'); append(meta.diff,'SosCode')
  elseif tool=='apply_patch' and meta.files and #meta.files>0 then
    for _,file in ipairs(meta.files) do append(file.relativePath or file.movePath or file.filePath or file.path,'SosPath'); append(file.patch or '', 'SosCode') end
  elseif tool=='bash' or tool=='wait' then
    if meta.exit~=nil then output=output:gsub('\n\n<shell_metadata>.-</shell_metadata>%s*$','') end
    append(meta.timedOut and 'Timed out' or 'Exit code: '..tostring(meta.exit or 'unknown'),
      (meta.timedOut or (meta.exit~=nil and meta.exit~=0)) and 'SosError' or 'SosValidated')
    append(output,'SosCode')
  elseif tool=='glob' then append(output,'SosPath')
  elseif tool=='grep' then
    local lines=vim.split(output,'\n',{plain=true})
    local count=meta.matches or tonumber((lines[1] or ''):match('^Found (%d+) matches$'))
    if lines[1]=='No files found' then count=0 end
    if count and #lines>0 then lines[1]=count..' Matches' end
    for _,line in ipairs(lines) do
      if line~='' then
        local prefix=line:match('^(%s+Line %d+:)')
        rows[#rows+1]={text=line,group=not prefix and line:match(':$') and 'SosPath' or nil,
          marks=prefix and {{first=0,last=#prefix,group='SosRead'}} or nil}
      end
    end
  elseif tool=='webfetch' then append(meta.url or args.url or '','SosPath'); append(output)
  else append(output) end
  if meta.truncated then append('Output truncated'..(meta.outputPath and ' · Full output: '..meta.outputPath or ''),'SosTool') end
  for _,attachment in ipairs(result.attachments or {}) do append('Attachment · '..tostring(attachment.mime),'SosMuted') end
  return rows
end
return M
