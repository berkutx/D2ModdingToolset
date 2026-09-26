#requires -Version 7.0
# Offline: actual extracted runner policies, no game/API/process launch.
# PID-log I/O uses only this test's temporary directory; no existing log is edited.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$path = Join-Path $repo 'tools/test/simturns-lobby-e2e.ps1'
$tokens=$null; $errors=$null
$ast = [Management.Automation.Language.Parser]::ParseFile($path,[ref]$tokens,[ref]$errors)
if ($errors.Count) { throw ($errors | Out-String) }
$functions = @{}
foreach ($node in $ast.FindAll({ param($item) $item -is [Management.Automation.Language.FunctionDefinitionAst] }, $false)) {
    $functions[$node.Name] = $node
}
foreach ($name in @('Property', 'Protect-UiSnapshot', 'ConvertTo-NativeReportedText', 'Get-PreparedPrompt',
    'Get-ExactEntryAction', 'Test-ConsumedEntryAppearance', 'Import-ExistingPopupObservers', 'Start-OwnedPair',
    'Get-LogLaunchBoundary', 'Assert-FreshOwnedLog', 'Get-OwnedLogEvidence', 'Copy-OwnedLogTail', 'Compare-ProtectedFileHashes')) {
    . ([scriptblock]::Create($functions[$name].Extent.Text))
}
$script:checks=0
function Check([bool]$condition,[string]$message) { $script:checks++; if(-not $condition) { throw $message } }
function Reject([scriptblock]$operation,[string]$pattern) {
    $caught=$false
    try { & $operation | Out-Null } catch { $caught=$_.Exception.Message -like $pattern }
    Check $caught "Expected terminal rejection: $pattern"
}
function Ui([string]$dialog,[string]$button,[string]$text='') {
    [pscustomobject]@{ dialog=$dialog; dialogReady=$true; dialogAppearance=13; dialogInstance=13
        widgets=@([pscustomobject]@{ name='TXT_INFO'; type='text'; state=[pscustomobject]@{ text=$text } })
        targets=@([pscustomobject]@{ dialog=$dialog; instance=42
            widgets=@([pscustomobject]@{ name=$button; type='button'; state=[pscustomobject]@{ enabled=$true } }) }) }
}
$expectedHost = 'Diligence 1.2.3 · áåç ðåéòèíãà' + "`n" + 'test2 '+[char]0x97+' Ýëüôû (õîñò)' + "`n" +
    'test1 '+[char]0x97+' Êëàíû' + "`n" + '1-é õîä: test2' + "`n" + 'ÎÕ: îáúåäèíåíèå íà äåíü 2' + "`n" + 'Ñãåíåðèðîâàòü êàðòó?'
$hostPrompt=Get-PreparedPrompt host 'Diligence 1.2.3'
Check ($hostPrompt -ceq $expectedHost) 'Host prompt differs from saved actual run004 bytes'
$joinPrompt=Get-PreparedPrompt join 'Diligence 1.2.3'
Check ($joinPrompt -ceq "Diligence 1.2.3`nÕîñò: test2`nÊàðòà ãîòîâà.`nÂîéòè â êîìíàòó?") 'Join prompt differs from saved actual bytes'
$action=Get-ExactEntryAction (Ui DLG_MESSAGE_BOX BTN_YES $hostPrompt) host host-offer $hostPrompt
Check ($action.Appearance -eq 13 -and $action.Owner -eq 42 -and $action.Button -ceq 'BTN_YES') 'Exact native owner was not preserved'
Check ($null -ne (Get-ExactEntryAction (Ui DLG_GENERATION_RESULT BTN_ACCEPT) host generation)) 'Generation control rejected'
$script:Consumed=@{ 'host|13|42|DLG_MESSAGE_BOX|BTN_YES'=$true }
Check (Test-ConsumedEntryAppearance (Ui DLG_MESSAGE_BOX BTN_YES $hostPrompt) host) 'Consumed prior prompt was not held for departure'
Check (-not (Test-ConsumedEntryAppearance (Ui DLG_MESSAGE_BOX BTN_YES $joinPrompt) join)) 'Consumed host prompt hid a join prompt'
$fresh=Ui DLG_MESSAGE_BOX BTN_YES 'unknown'; $fresh.dialogAppearance=14; $fresh.dialogInstance=14
Check (-not (Test-ConsumedEntryAppearance $fresh host)) 'Consumed appearance hid a fresh unknown popup'
Reject { Get-ExactEntryAction (Ui DLG_MESSAGE_BOX BTN_YES 'Abort') host host-offer $hostPrompt } '*Unknown or mismatched*'
Reject { Get-ExactEntryAction (Ui DLG_MESSAGE_BOX BTN_YES $hostPrompt) join host-offer $hostPrompt } '*another role*'
Reject { Get-ExactEntryAction (Ui DLG_MESSAGE_BOX BTN_YES $joinPrompt) join join-offer ($joinPrompt+' ') } '*Unknown or mismatched*'
Reject { Get-ExactEntryAction (Ui DLG_MESSAGE_BOX BTN_YES $hostPrompt) host generation } '*Unknown or mismatched*'
Reject { Get-ExactEntryAction (Ui DLG_MESSAGE_BOX BTN_YES 'Íà÷àëî çàäàíèÿ, äåíü 1') join join-offer $joinPrompt } '*Unknown or mismatched*'
$bad=Ui DLG_MESSAGE_BOX BTN_YES $hostPrompt; $bad.dialogReady='true'
Reject { Get-ExactEntryAction $bad host host-offer $hostPrompt } '*readiness shape*'
$bad=Ui DLG_MESSAGE_BOX BTN_YES $hostPrompt; $bad.targets[0].widgets[0].state.enabled='true'
Reject { Get-ExactEntryAction $bad host host-offer $hostPrompt } '*explicitly enabled*'
$bad=Ui DLG_MESSAGE_BOX BTN_YES $hostPrompt; $bad.targets=@($bad.targets[0],$bad.targets[0])
Reject { Get-ExactEntryAction $bad host host-offer $hostPrompt } '*exactly one native owner*'
$bad=Ui DLG_MESSAGE_BOX BTN_YES $hostPrompt; $bad.dialogInstance=12
Reject { Get-ExactEntryAction $bad host host-offer $hostPrompt } '*appearance is invalid*'
Reject { Get-PreparedPrompt host "Diligence`nforeign" } '*one exact line*'
$secretUi=[pscustomobject]@{ widgets=@([pscustomobject]@{ name='EDIT_PASSWORD'; type='edit'; state=[pscustomobject]@{ text='offline-canary' } }); targets=@() }
Protect-UiSnapshot $secretUi
Check (($secretUi | ConvertTo-Json -Depth 8) -notmatch 'offline-canary') 'Snapshot leaked edit text'
Check (-not $functions.ContainsKey('Advance-OwnedStartup')) 'PowerShell startup popup actor reintroduced'
Check (-not $functions.ContainsKey('Apply-Command')) 'Manual action JSON channel reintroduced'
$source=$ast.Extent.Text
Check ($source -notmatch 'action-\{|action-001|Set-EditText|EDIT_NAME|BTN_RACE|BTN_LORD') 'Manual or leader/race action reintroduced'
$textWrites=@($ast.FindAll({ param($node)
    $node -is [Management.Automation.Language.CommandAst] -and $node.GetCommandName() -eq 'Set-SecretEditText'
},$true))
Check ($textWrites.Count -eq 2) 'Only the two login secret text operations may exist'
Check (@($textWrites | Where-Object { $_.Extent.Text -notmatch 'DLG_LOGIN_ACCOUNT EDIT_(ACCOUNT_NAME|PASSWORD)' }).Count -eq 0) 'Text input escaped login'
Check ($source.Contains("'SCRIPTED_POPUPS_LOBBY'")) 'Native lobby popup scope missing'
Check ($source.Contains('$psi.Environment[''D2_LOBBY_HOST_PASSWORD'']') -and
    -not ($functions['Invoke-OwnedPreparation'].Extent.Text -match 'ArgumentList.Add\([^\n]*(PASSWORD|credentials)')) 'Secret passed as process argument'

# Verify actual existing observer imports retain their parameter lists and scopes.
Import-ExistingPopupObservers (Join-Path $repo 'tools/test/simturns-production-poc.ps1')
$mockLog=Join-Path $repo 'artifacts/mss32_12345.log'
$script:ClientLogInitialLengths=@{}; $script:ClientLogOwnedProcessIds=@{}
$script:ClientLogInitialLengths[$mockLog]=[long]0; $script:ClientLogOwnedProcessIds[$mockLog]=12345
Check ((Get-ClientLogBaseline $mockLog) -eq 0) 'Imported baseline parameters/scope broken'
$observer=New-LiteralStartupPopupService host $mockLog
Check ($observer.Role -ceq 'host' -and $observer.Appearances.Count -eq 0) 'Imported native observer scope broken'
Reject { Get-ClientLogBaseline (Join-Path $repo 'artifacts/mss32_999.log') } '*not bound to a process*'
foreach($name in @('New-LiteralStartupPopupService','Invoke-LiteralPersistentStartupPopupTick','Assert-PairedStartupActionsRelease')) {
    Check ((Get-Command $name).ScriptBlock.ToString() -notmatch 'Invoke-Button|Set-Edit|Set-Secret|/api/ui/invoke') "Existing observer $name unexpectedly acts"
}

# Reused PID: actual append-log boundary + original reader must exclude the old
# PASS/fault/native-marker prefix; only new bytes can prove this run's behavior.
$scratch = [IO.Path]::GetFullPath((Join-Path ([IO.Path]::GetTempPath()) ('d2-lobby-log-test-' + [guid]::NewGuid().ToString('N'))))
[void](New-Item -ItemType Directory -Path $scratch)
$logPath = Join-Path $scratch 'mss32_12345.log'
$tailPath = Join-Path $scratch 'host.mss32.log'
try {
    $GameDir=$scratch; $ArtifactDir=$scratch; $ExpectedMergeDay=2
    $oldText="bootstrap operational release applied; strict independent turns are operational`n[simturns] terminal fault: previous run`n[testdrv][scripted-popup] malformed previous run marker`n"
    [IO.File]::WriteAllText($logPath,$oldText,[Text.UTF8Encoding]::new($false))
    $boundary=Get-LogLaunchBoundary
    $script:Clients=@{host=[pscustomobject]@{Id=12345}}
    $script:ClientLogs=@{}; $script:ClientLogInitialLengths=@{}; $script:ClientLogOwnedProcessIds=@{}
    [IO.File]::AppendAllText($logPath,"fresh launch`n",[Text.UTF8Encoding]::new($false))
    Assert-FreshOwnedLog host $boundary
    Check ((Get-ClientLogBaseline $logPath) -eq [Text.Encoding]::UTF8.GetByteCount($oldText)) 'Reused PID lost its prelaunch byte boundary'
    $evidence=Get-OwnedLogEvidence host
    Check (-not $evidence.bootstrapOperational -and -not $evidence.fault) 'Old PASS/fault bytes leaked across launch boundary'
    Check ((Read-ClientLogLines $logPath) -ceq 'fresh launch') 'Existing PID reader returned prelaunch content'
    $observer=New-LiteralStartupPopupService host $logPath
    Check (@(Invoke-LiteralPersistentStartupPopupTick $observer).Count -eq 0) 'Old native popup markers leaked across launch boundary'
    [IO.File]::AppendAllText($logPath,"bootstrap operational release applied; strict independent turns are operational`n",[Text.UTF8Encoding]::new($false))
    Check (Get-OwnedLogEvidence host).bootstrapOperational 'New bootstrap marker was not accepted after launch boundary'
    Copy-OwnedLogTail host
    $tail=[IO.File]::ReadAllText($tailPath)
    Check ($tail.StartsWith("fresh launch`n") -and -not $tail.Contains('previous run')) 'Artifact log copied stale prefix'
    Check ([IO.File]::ReadAllText($logPath).StartsWith($oldText)) 'Source log prefix was changed or cleared'
    [IO.File]::WriteAllText($logPath,"short`n",[Text.UTF8Encoding]::new($false))
    Reject { Read-ClientLogLines $logPath } '*shorter than its launch boundary*'
    Reject { Copy-OwnedLogTail host } '*truncated*'
} finally {
    # Exact test-owned files only; no recursive deletion or computed broad target.
    if ([IO.File]::Exists($logPath)) { [IO.File]::Delete($logPath) }
    if ([IO.File]::Exists($tailPath)) { [IO.File]::Delete($tailPath) }
    [IO.Directory]::Delete($scratch, $false)
}
$hashProof=Compare-ProtectedFileHashes @{'Disciple.ini'='before';'Scripts/settings.lua'='same';'missing.lua'=$null} @{'Disciple.ini'='after';'Scripts/settings.lua'='same';'missing.lua'=$null}
Check (-not $hashProof.unchanged -and ($hashProof.changedFiles -join ',') -ceq 'Disciple.ini') 'INI hash change was not strict and exactly named'
Check ($hashProof.before['Disciple.ini'] -ceq 'before' -and $hashProof.after['Disciple.ini'] -ceq 'after') 'Before/after hashes were not preserved'
Check (-not (Compare-ProtectedFileHashes @{'missing.lua'=$null} @{'missing.lua'='created'}).unchanged) 'New protected file escaped hash comparison'

# Actual pair orchestration with native observer/actor boundaries mocked, simulated clock.
function Get-PairUtcNow { $script:Now }
function Start-Sleep([int]$Milliseconds) { $script:Now=$script:Now.AddMilliseconds($Milliseconds) }
function Save-PairReceipt([string]$Stage,$Proof=$null) { $script:Stages.Add($Stage) }
function Assert-PairProgress { if($script:Fault) { throw 'mock production fault' } }
function Invoke-ReadyRoomStart([string]$Role) { $script:Starts.Add($Role); if($Role -eq 'join') { $script:JoinStartAt=$script:Now } }
function Get-RoleState([string]$Role) { [pscustomobject]@{ reachedStrategic=$true; uiSeq=10 } }
function Get-GameUiSnapshot([string]$Role) { [pscustomobject]@{ dialogReady=$true; mapLoaded=$true; startupActionsHeld=$true } }
function Request-DelayedHostStartupRelease { $script:Releases++; $script:NameTxAt=$script:Now; $script:logEvidence.host.startupLeaderNameSent=$true }
function Reset-Pair([string]$Mode) {
    $script:PairStartup=$Mode; $script:JoinStartupDelaySeconds=8; $script:PairStartClaimed=$false; $script:PairMapsObserved=$false
    $script:PreparationConsent=@{host=$true;join=$true}; $script:GenerationAccepted=$true
    $script:Now=[DateTime]::Parse('2026-09-26T00:00:00Z'); $script:Starts=[Collections.Generic.List[string]]::new()
    $script:Stages=[Collections.Generic.List[string]]::new(); $script:logEvidence=@{host=@{startupLeaderNameSent=$false}}
    $script:Releases=0; $script:NameTxAt=$null; $script:JoinStartAt=$null; $script:Fault=$false
}
Reset-Pair Normal
Start-OwnedPair
Check (($script:Starts -join ',') -ceq 'host,join') 'Normal did not start one exact host then join'
Check ($script:PairMapsObserved -and $script:Releases -eq 0) 'Normal bypassed existing paired release'
Reject { Start-OwnedPair } '*already consumed*'
Reset-Pair DelayedJoin
Start-OwnedPair
Check ($script:Releases -eq 1 -and ($script:JoinStartAt-$script:NameTxAt).TotalSeconds -ge 8) 'Delayed repro lost one-shot native release/post-TX interval'
Reset-Pair Normal
$script:PreparationConsent.join=$false
Reject { Start-OwnedPair } '*explicit host/join consent*'
Check ($script:Starts.Count -eq 0) 'Missing consent started a client'
Reset-Pair DelayedJoin
$script:Fault=$true
Reject { Start-OwnedPair } '*mock production fault*'
Check (($script:Starts -join ',') -ceq 'host') 'Fault allowed join/retry'
$nodeAssignment = @($ast.FindAll({ param($item)
    $item -is [Management.Automation.Language.AssignmentStatementAst] -and
        $item.Left.Extent.Text -ceq '$psi.FileName'
}, $true))
Check ($nodeAssignment.Count -eq 1) 'Node helper executable assignment is not unique'
& {
    function Get-Command { @([pscustomobject]@{Source='C:\\first\\node.exe'}, [pscustomobject]@{Source='C:\\second\\node.exe'}) }
    $psi = [Diagnostics.ProcessStartInfo]::new()
    . ([scriptblock]::Create($nodeAssignment[0].Extent.Text))
    Check ($psi.FileName -ceq 'C:\\first\\node.exe') 'Multiple PATH matches were concatenated into one executable'
}
"PASS: $script:checks offline lobby E2E checks; no processes, APIs or game callbacks; temporary log fixture cleaned"
