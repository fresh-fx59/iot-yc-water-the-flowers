let selectedValve = null;
let isSequentialRunning = false;
let currentSequenceIndex = 0;
let sequenceValves = [];

function selectValve(valveNum, button) {
  // Clear previous selection
  document.querySelectorAll('.valve-btn').forEach(btn => btn.classList.remove('selected'));
  // Mark current as selected
  button.classList.add('selected');
  selectedValve = valveNum;
  addLog(`Valve ${valveNum} selected for single watering`, 'info');
}

function startWateringOne() {
  if (selectedValve === null) {
    addLog('Please select a valve first', 'error');
    return;
  }
  addLog(`Starting watering for Valve ${selectedValve}...`, 'info');
  fetch(`/api/water?valve=${selectedValve}`)
    .then(r => r.json())
    .then(data => {
      if (data.success) {
        addLog(`✓ Valve ${selectedValve} watering started`, 'success');
      } else {
        addLog(`✗ Failed to start Valve ${selectedValve}: ${data.message}`, 'error');
      }
    })
    .catch(e => addLog(`✗ Error: ${e}`, 'error'));
}

function stopWateringOne() {
  if (selectedValve === null) {
    addLog('No valve selected', 'error');
    return;
  }
  addLog(`Stopping watering for Valve ${selectedValve}...`, 'info');
  fetch(`/api/stop?valve=${selectedValve}`)
    .then(r => r.json())
    .then(data => {
      if (data.success) {
        addLog(`✓ Valve ${selectedValve} stopped`, 'success');
      } else {
        addLog(`✗ Failed to stop Valve ${selectedValve}`, 'error');
      }
    })
    .catch(e => addLog(`✗ Error: ${e}`, 'error'));
}

function startSequentialWatering() {
  sequenceValves = [];
  for (let i = 1; i <= 6; i++) {
    if (document.getElementById(`seq-valve-${i}`).checked) {
      sequenceValves.push(i);
    }
  }

  if (sequenceValves.length === 0) {
    addLog('Please select at least one valve for sequence', 'error');
    return;
  }

  isSequentialRunning = true;
  currentSequenceIndex = 0;
  document.getElementById('startSeqBtn').disabled = true;
  addLog(`Starting sequential watering: Valves [${sequenceValves.join(', ')}]`, 'info');
  
  processNextInSequence();
}

function processNextInSequence() {
  if (!isSequentialRunning || currentSequenceIndex >= sequenceValves.length) {
    isSequentialRunning = false;
    document.getElementById('startSeqBtn').disabled = false;
    addLog('Sequential watering completed', 'success');
    return;
  }

  const valve = sequenceValves[currentSequenceIndex];
  addLog(`[Sequence ${currentSequenceIndex + 1}/${sequenceValves.length}] Starting Valve ${valve}...`, 'info');
  
  fetch(`/api/water?valve=${valve}`)
    .then(r => r.json())
    .then(data => {
      if (data.success) {
        addLog(`✓ Valve ${valve} started, waiting for completion...`, 'success');
        // Wait for valve to complete (check every 2 seconds)
        checkValveCompletion(valve, 0);
      } else {
        addLog(`✗ Failed to start Valve ${valve}`, 'error');
        currentSequenceIndex++;
        processNextInSequence();
      }
    })
    .catch(e => {
      addLog(`✗ Error: ${e}`, 'error');
      currentSequenceIndex++;
      processNextInSequence();
    });
}

function checkValveCompletion(valve, attempts) {
  if (!isSequentialRunning) return;
  
  if (attempts > 300) { // 10 minutes max per valve
    addLog(`Valve ${valve} timeout, moving to next...`, 'error');
    currentSequenceIndex++;
    processNextInSequence();
    return;
  }

  fetch('/api/status')
    .then(r => r.json())
    .then(data => {
      const valveStatus = data.valves.find(v => v.id === valve - 1);
      if (valveStatus && valveStatus.phase === 'idle') {
        addLog(`✓ Valve ${valve} completed (${valveStatus.state})`, 'success');
        currentSequenceIndex++;
        processNextInSequence();
      } else {
        setTimeout(() => checkValveCompletion(valve, attempts + 1), 2000);
      }
    })
    .catch(() => setTimeout(() => checkValveCompletion(valve, attempts + 1), 2000));
}

function stopSequentialWatering() {
  isSequentialRunning = false;
  document.getElementById('startSeqBtn').disabled = false;
  addLog('Sequential watering stopped', 'info');
  fetch('/api/stop?valve=all')
    .then(r => r.json())
    .then(data => {
      if (data.success) {
        addLog('✓ All valves stopped', 'success');
      }
    })
    .catch(e => addLog(`✗ Error: ${e}`, 'error'));
}

function updateStatus() {
  fetch('/api/status')
    .then(r => r.json())
    .then(data => {
      // Update pump status
      const pumpStatus = document.getElementById('pumpStatus');
      const pumpText = document.getElementById('pumpStatusText');
      if (data.pump === 'on') {
        pumpStatus.classList.add('active');
        pumpStatus.classList.remove('inactive');
        pumpText.textContent = 'ON';
      } else {
        pumpStatus.classList.remove('active');
        pumpStatus.classList.add('inactive');
        pumpText.textContent = 'OFF';
      }
      
      // Update system status
      const systemStatus = document.getElementById('systemStatus');
      const systemText = document.getElementById('systemStatusText');
      const activeValves = data.valves.filter(v => v.state === 'open').length;
      if (activeValves > 0) {
        systemStatus.classList.add('active');
        systemStatus.classList.remove('inactive');
        systemText.textContent = `Watering (${activeValves} valve${activeValves > 1 ? 's' : ''})`;
      } else {
        systemStatus.classList.remove('active');
        systemStatus.classList.add('inactive');
        systemText.textContent = 'Idle';
      }
    })
    .catch(e => console.error('Status update error:', e));
}

function addLog(message, type = 'info') {
  const log = document.getElementById('statusLog');
  const entry = document.createElement('div');
  entry.className = `log-entry ${type}`;
  
  const time = new Date().toLocaleTimeString();
  entry.innerHTML = `<span class="log-timestamp">[${time}]</span> ${message}`;
  
  log.insertBefore(entry, log.firstChild);
  
  // Keep only last 50 entries
  while (log.children.length > 50) {
    log.removeChild(log.lastChild);
  }
}

// Update status every 2 seconds
setInterval(updateStatus, 2000);
updateStatus();