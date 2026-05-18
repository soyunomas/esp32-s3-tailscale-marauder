(function(){
  var macros=[];
  var nameInput=document.getElementById('usbHidMacroName');
  var layoutInput=document.getElementById('usbHidLayout');
  var scriptInput=document.getElementById('usbHidMacroScript');
  var executeBtn=document.getElementById('usbHidExecute');
  var stopBtn=document.getElementById('usbHidStop');
  var panicBtn=document.getElementById('usbHidPanic');
  var saveBtn=document.getElementById('usbHidSave');
  var clearBtn=document.getElementById('usbHidClear');
  var listEl=document.getElementById('usbHidMacroList');
  var exampleListEl=document.getElementById('usbHidExampleList');
  var countEl=document.getElementById('usbHidMacroCount');
  var statusEl=document.getElementById('usbHidStatus');
  var stateEl=document.querySelector('.usb-hid-state');
  var currentId=0;
  var statusTimer=0;
  var errorLine=0;
  var defaultLayout='es';
  var examples=[
    {
      name:'Windows shutdown',
      script:'GUI r\nDELAY 250\nSTRING shutdown /s /t 0\nENTER'
    },
    {
      name:'Windows restart',
      script:'GUI r\nDELAY 250\nSTRING shutdown /r /t 0\nENTER'
    },
    {
      name:'Open Run dialog',
      script:'GUI r'
    },
    {
      name:'Home server maintenance',
      script:'GUI r\nDELAY 250\nSTRING cmd /k echo Replace this template with your home server maintenance commands\nENTER'
    },
    {
      name:'Linux shutdown',
      script:'CTRL ALT t\nRELEASE ALL\nDELAY 1000\nSTRING systemctl poweroff\nENTER'
    },
    {
      name:'Linux restart',
      script:'CTRL ALT t\nRELEASE ALL\nDELAY 1000\nSTRING systemctl reboot\nENTER'
    },
    {
      name:'Linux open terminal',
      script:'CTRL ALT t\nRELEASE ALL'
    },
    {
      name:'Linux maintenance template',
      script:'CTRL ALT t\nRELEASE ALL\nDELAY 1000\nSTRING echo Replace this template with your Linux maintenance commands\nENTER'
    }
  ];

  function authHeaders(extra){
    var headers=extra||{};
    var auth=sessionStorage.getItem('auth')||'';
    if(auth) headers.Authorization=auth;
    return headers;
  }

  function setStatus(text){
    if(statusEl) statusEl.textContent=text;
  }

  function setState(text){
    if(stateEl) stateEl.textContent=text;
  }

  function clearErrorLine(){
    errorLine=0;
    if(scriptInput){
      scriptInput.classList.remove('error-line');
      scriptInput.style.removeProperty('--usb-hid-error-y');
      scriptInput.style.removeProperty('--usb-hid-line-height');
    }
  }

  function getEditorLineMetrics(){
    var style=getComputedStyle(scriptInput);
    var fontSize=parseFloat(style.fontSize)||13;
    var lineHeight=parseFloat(style.lineHeight)||0;
    if(!lineHeight||lineHeight<=4) lineHeight=fontSize*(lineHeight||1.45);
    return {
      lineHeight:lineHeight,
      paddingTop:parseFloat(style.paddingTop)||0
    };
  }

  function updateErrorLine(){
    if(!scriptInput||!errorLine) return;
    var metrics=getEditorLineMetrics();
    var lineHeight=metrics.lineHeight;
    var paddingTop=metrics.paddingTop;
    var y=paddingTop+((errorLine-1)*lineHeight)-scriptInput.scrollTop;
    scriptInput.style.setProperty('--usb-hid-line-height',lineHeight+'px');
    scriptInput.style.setProperty('--usb-hid-error-y',y+'px');
  }

  function markErrorLine(line){
    line=Number(line)||0;
    if(!scriptInput||line<1) return;
    errorLine=line;
    var metrics=getEditorLineMetrics();
    var lineHeight=metrics.lineHeight;
    var paddingTop=metrics.paddingTop;
    var lineTop=paddingTop+((line-1)*lineHeight);
    if(lineTop<scriptInput.scrollTop||lineTop>scriptInput.scrollTop+scriptInput.clientHeight-lineHeight){
      scriptInput.scrollTop=Math.max(0,lineTop-(scriptInput.clientHeight/2));
    }
    scriptInput.classList.add('error-line');
    updateErrorLine();
  }

  function parseErrorLine(text){
    var match=String(text||'').match(/\bline\s+(\d+)\b/i);
    return match?Number(match[1]):0;
  }

  function cleanErrorText(text){
    text=String(text||'Execution failed').replace(/<[^>]*>/g,' ');
    return text.replace(/\s+/g,' ').trim()||'Execution failed';
  }

  function formatExecutorStatus(status){
    var name=status.macro_name||'Unsaved Macro';
    var mode=status.dry_run?'Dry run':'USB HID';
    if(status.state==='running'||status.state==='stopping'){
      var progress=status.total_lines?(' line '+status.current_line+'/'+status.total_lines):'';
      return mode+': '+name+progress+' - '+(status.message||status.state);
    }
    if(status.state==='done'||status.state==='error'){
      return mode+': '+(status.message||status.state)+' - '+name;
    }
    return 'Idle';
  }

  function updateExecutionButtons(running){
    if(executeBtn) executeBtn.disabled=!!running;
    if(stopBtn) stopBtn.disabled=!running;
    if(panicBtn) panicBtn.disabled=false;
  }

  function stopStatusPolling(){
    if(statusTimer){
      clearInterval(statusTimer);
      statusTimer=0;
    }
  }

  function loadStatus(){
    return fetch('/api/usb-hid/status',{headers:authHeaders()})
    .then(function(r){
      if(!r.ok) throw new Error('status failed');
      return r.json();
    }).then(function(status){
      var running=status.state==='running'||status.state==='stopping';
      setStatus(formatExecutorStatus(status));
      setState(status.dry_run?'Dry run':'USB HID');
      updateExecutionButtons(running);
      if(!running) stopStatusPolling();
      return status;
    }).catch(function(){
      setStatus('Execution status unavailable');
      setState('Unavailable');
      updateExecutionButtons(false);
      stopStatusPolling();
    });
  }

  function startStatusPolling(){
    stopStatusPolling();
    loadStatus();
    statusTimer=setInterval(loadStatus,1000);
  }

  function normalizeName(name){
    return (name||'').trim().slice(0,32);
  }

  function currentMacro(){
    return {
      id:currentId,
      name:normalizeName(nameInput.value)||'Untitled Macro',
      layout:(layoutInput&&layoutInput.value)||defaultLayout,
      script:(scriptInput.value||'').trim()
    };
  }

  function renderMacros(){
    countEl.textContent=String(macros.length);
    listEl.innerHTML='';
    if(!macros.length){
      listEl.innerHTML='<div class="usb-hid-empty">No saved macros</div>';
      return;
    }
    macros.forEach(function(macro){
      var row=document.createElement('div');
      row.className='usb-hid-macro';

      var name=document.createElement('span');
      name.className='usb-hid-macro-name';
      name.textContent=macro.name||'Untitled Macro';
      row.appendChild(name);

      var actions=document.createElement('div');
      actions.className='usb-hid-macro-actions';

      var load=document.createElement('button');
      load.type='button';
      load.className='usb-hid-icon-btn';
      load.textContent='Load';
      load.addEventListener('click',function(){loadMacro(macro)});
      actions.appendChild(load);

      var run=document.createElement('button');
      run.type='button';
      run.className='usb-hid-icon-btn';
      run.textContent='Run';
      run.addEventListener('click',function(){
        loadMacro(macro);
        executeMacro(macro);
      });
      actions.appendChild(run);

      var rename=document.createElement('button');
      rename.type='button';
      rename.className='usb-hid-icon-btn';
      rename.textContent='Rename';
      rename.addEventListener('click',function(){renameMacro(macro)});
      actions.appendChild(rename);

      var del=document.createElement('button');
      del.type='button';
      del.className='usb-hid-icon-btn';
      del.textContent='Del';
      del.addEventListener('click',function(){deleteMacro(macro)});
      actions.appendChild(del);

      row.appendChild(actions);
      listEl.appendChild(row);
    });
  }

  function renderExamples(){
    if(!exampleListEl) return;
    exampleListEl.innerHTML='';
    examples.forEach(function(example){
      var row=document.createElement('div');
      row.className='usb-hid-macro';

      var name=document.createElement('span');
      name.className='usb-hid-macro-name';
      name.textContent=example.name;
      row.appendChild(name);

      var actions=document.createElement('div');
      actions.className='usb-hid-macro-actions';

      var load=document.createElement('button');
      load.type='button';
      load.className='usb-hid-icon-btn';
      load.textContent='Load';
      load.addEventListener('click',function(){loadExample(example)});
      actions.appendChild(load);

      row.appendChild(actions);
      exampleListEl.appendChild(row);
    });
  }

  function loadMacro(macro){
    currentId=macro.id||0;
    nameInput.value=macro.name||'';
    if(layoutInput) layoutInput.value=macro.layout||defaultLayout;
    scriptInput.value=macro.script||'';
    setStatus('Loaded: '+(macro.name||'Untitled Macro'));
  }

  function loadExample(example){
    currentId=0;
    nameInput.value=example.name||'';
    if(layoutInput) layoutInput.value=example.layout||defaultLayout;
    scriptInput.value=example.script||'';
    setStatus('Loaded example: '+(example.name||'Untitled Macro'));
  }

  function loadMacros(){
    fetch('/api/usb-hid/macros',{headers:authHeaders()})
    .then(function(r){
      if(!r.ok) throw new Error('load failed');
      return r.json();
    }).then(function(data){
      macros=Array.isArray(data.macros)?data.macros:[];
      renderMacros();
      setStatus('Idle');
    }).catch(function(){
      macros=[];
      renderMacros();
      setStatus('Macro storage unavailable');
    });
  }

  function saveMacro(){
    var macro=currentMacro();
    if(!macro.script){
      setStatus('Macro script is empty');
      scriptInput.focus();
      return;
    }
    saveBtn.disabled=true;
    fetch('/api/usb-hid/macros',{
      method:'POST',
      headers:authHeaders({'Content-Type':'application/json'}),
      body:JSON.stringify(macro)
    }).then(function(r){
      if(!r.ok) throw new Error('save failed');
      return r.json();
    }).then(function(saved){
      currentId=saved.id||0;
      setStatus('Saved: '+(saved.name||macro.name));
      loadMacros();
    }).catch(function(){
      setStatus('Save failed');
    }).finally(function(){
      saveBtn.disabled=false;
    });
  }

  function deleteMacro(macro){
    fetch('/api/usb-hid/macros/'+macro.id,{
      method:'DELETE',
      headers:authHeaders()
    }).then(function(r){
      if(!r.ok) throw new Error('delete failed');
      setStatus('Deleted: '+(macro.name||'Untitled Macro'));
      if(currentId===macro.id) clearEditor(false);
      loadMacros();
    }).catch(function(){
      setStatus('Delete failed');
    });
  }

  function renameMacro(macro){
    var next=prompt('Rename macro', macro.name||'Untitled Macro');
    if(next===null) return;
    next=normalizeName(next);
    if(!next){
      setStatus('Macro name is empty');
      return;
    }
    var updated={id:macro.id,name:next,layout:macro.layout||defaultLayout,script:macro.script||''};
    fetch('/api/usb-hid/macros/'+macro.id,{
      method:'PUT',
      headers:authHeaders({'Content-Type':'application/json'}),
      body:JSON.stringify(updated)
    }).then(function(r){
      if(!r.ok) throw new Error('rename failed');
      return r.json();
    }).then(function(saved){
      if(currentId===macro.id) nameInput.value=saved.name||next;
      setStatus('Renamed: '+(saved.name||next));
      loadMacros();
    }).catch(function(){
      setStatus('Rename failed');
    });
  }

  function executeMacro(macro){
    macro=macro||currentMacro();
    clearErrorLine();
    if(!macro.script){
      setStatus('Macro script is empty');
      scriptInput.focus();
      return;
    }
    updateExecutionButtons(true);
    fetch('/api/usb-hid/execute',{
      method:'POST',
      headers:authHeaders({'Content-Type':'application/json'}),
      body:JSON.stringify({name:macro.name,layout:macro.layout||defaultLayout,script:macro.script})
    }).then(function(r){
      if(!r.ok){
        return r.text().then(function(text){
          var error=new Error(cleanErrorText(text));
          error.line=parseErrorLine(text);
          throw error;
        });
      }
      return r.json();
    }).then(function(){
      setStatus('Queued: '+(macro.name||'Untitled Macro'));
      startStatusPolling();
    }).catch(function(error){
      if(error&&error.line) markErrorLine(error.line);
      setStatus('Execution failed: '+cleanErrorText(error&&error.message));
      updateExecutionButtons(false);
    });
  }

  function stopExecution(){
    if(stopBtn) stopBtn.disabled=true;
    fetch('/api/usb-hid/stop',{
      method:'POST',
      headers:authHeaders()
    }).then(function(r){
      if(!r.ok) throw new Error('stop failed');
      setStatus('Stopping');
      startStatusPolling();
    }).catch(function(){
      setStatus('Stop failed');
      loadStatus();
    });
  }

  function panicStop(){
    if(panicBtn) panicBtn.disabled=true;
    if(stopBtn) stopBtn.disabled=true;
    fetch('/api/usb-hid/panic',{
      method:'POST',
      headers:authHeaders()
    }).then(function(r){
      if(!r.ok) throw new Error('panic failed');
      setStatus('Emergency stop sent');
      startStatusPolling();
    }).catch(function(){
      setStatus('Emergency stop failed');
      loadStatus();
    }).finally(function(){
      if(panicBtn) panicBtn.disabled=false;
    });
  }

  function clearEditor(focus){
    currentId=0;
    nameInput.value='';
    if(layoutInput) layoutInput.value=defaultLayout;
    scriptInput.value='';
    setStatus('Idle');
    if(focus!==false) nameInput.focus();
  }

  if(executeBtn) executeBtn.addEventListener('click',function(){executeMacro()});
  if(stopBtn) stopBtn.addEventListener('click',stopExecution);
  if(panicBtn) panicBtn.addEventListener('click',panicStop);
  if(saveBtn) saveBtn.addEventListener('click',saveMacro);
  if(clearBtn) clearBtn.addEventListener('click',function(){clearEditor(true)});
  if(scriptInput){
    scriptInput.addEventListener('input',clearErrorLine);
    scriptInput.addEventListener('scroll',updateErrorLine);
  }
  window.usbHidLoadMacros=loadMacros;
  updateExecutionButtons(false);
  renderMacros();
  renderExamples();
  loadStatus();
})();
