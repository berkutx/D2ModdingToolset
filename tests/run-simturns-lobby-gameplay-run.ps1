#requires -Version 7.0
# Offline tests of the real extracted gameplay orchestration. Only the HttpClient
# constructor is replaced; its requests are in-memory tasks, never network I/O.
param(
    # Optional, read-only replay of an explicitly selected finished run. CI uses
    # synthetic fixtures and does not depend on ignored local evidence files.
    [string]$ReplayArtifactDirectory
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$path=Join-Path $repo 'tools/test/simturns-lobby-gameplay-run.ps1'
$tokens=$null; $errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile($path,[ref]$tokens,[ref]$errors)
if($errors.Count) { throw ($errors | Out-String) }
$functions=@{}
foreach($node in $ast.FindAll({param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst]},$false)) {
    if($functions.ContainsKey($node.Name)) { throw "Duplicate function $($node.Name)" }
    $functions[$node.Name]=$node
}
$policyPath=Join-Path $repo 'tools/test/simturns-lobby-gameplay.ps1'
$policyAst=[Management.Automation.Language.Parser]::ParseFile($policyPath,[ref]$tokens,[ref]$errors)
if($errors.Count) { throw ($errors | Out-String) }
$integerHelper=@($policyAst.FindAll({param($n)
    $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -ceq 'Get-LobbyGameplayInteger'
},$false))
if($integerHelper.Count -ne 1) { throw 'Gameplay integer policy is not unique' }
. ([scriptblock]::Create($integerHelper[0].Extent.Text))
$script:checks=0
function Check([bool]$Condition,[string]$Message) { $script:checks++; if(-not $Condition) { throw $Message } }
function Reject([scriptblock]$Operation,[string]$Pattern='*',[string]$Context='') {
    $caught=$false
    try { & $Operation | Out-Null } catch { $caught=$_.Exception.Message -like $Pattern }
    Check $caught "Expected terminal rejection: $Pattern $Context"
}
function Copy-Shape($Value) { $Value | ConvertTo-Json -Depth 30 | ConvertFrom-Json }
function Read-Query([string]$Uri) {
    $query=@{}
    foreach($field in ([uri]$Uri).Query.TrimStart('?').Split('&')) {
        $parts=$field.Split('=',2)
        if($parts.Count -ne 2 -or $query.ContainsKey($parts[0])) { throw 'Malformed or duplicate query field' }
        $query[$parts[0]]=[uri]::UnescapeDataString($parts[1])
    }
    return $query
}
class OfflineLobbyHttpClient {
    [TimeSpan]$Timeout
    [bool]$Disposed=$false
    [string]$FailRole=''
    [hashtable]$Completions
    [Collections.Generic.List[object]]$Requests
    [Collections.Generic.List[string]]$Events
    OfflineLobbyHttpClient([hashtable]$completions,[Collections.Generic.List[string]]$events) {
        $this.Completions=$completions; $this.Events=$events
        $this.Requests=[Collections.Generic.List[object]]::new()
    }
    [object]PostAsync([string]$uri,[object]$content) {
        $query=Read-Query $uri
        $role=$query.role
        $this.Requests.Add([pscustomobject]@{uri=$uri;query=$query;body=$content.ReadAsStringAsync().GetAwaiter().GetResult()})
        $this.Events.Add("post-$role")
        if($this.FailRole -ceq $role) { throw "mock ambiguous $role send" }
        return $this.Completions[$role].Task
    }
    [void]Dispose() { $this.Disposed=$true }
}

# Refuse to load the sender unless its sole network constructor can be replaced
# exactly. All other validation, URI construction, loops and catch/finally logic
# execute verbatim from the actual function.
$sender=$functions['Start-LobbyMovePair']
$constructors=@($sender.Body.FindAll({param($n)
    $n -is [Management.Automation.Language.InvokeMemberExpressionAst] -and
        $n.Extent.Text -ceq '[Net.Http.HttpClient]::new()'
},$true))
Check ($constructors.Count -eq 1) 'Move sender must have exactly one recognized HttpClient constructor'
$senderText=$sender.Extent.Text.Replace($constructors[0].Extent.Text,'(New-OfflineLobbyHttpClient)')
Check (-not $senderText.Contains('[Net.Http.HttpClient]')) 'Unmocked network constructor remains'
. ([scriptblock]::Create($senderText))
foreach($name in @('Read-LobbyMoveReceipts','Invoke-LobbyEndTurnPair','Invoke-LobbyConcurrentBattleMerge')) {
    . ([scriptblock]::Create($functions[$name].Extent.Text))
}

$script:RelayBase='http://offline.invalid'
$mock=@{}
function Reset-Moves {
    $mock.Clear()
    $mock.events=[Collections.Generic.List[string]]::new()
    $mock.saved=[Collections.Generic.List[object]]::new()
    $mock.worlds=@{}; $mock.moves=@{}; $mock.bindings=@{}; $mock.completions=@{}
    foreach($role in @('host','join')) {
        $number=if($role -ceq 'host'){1}else{2}
        $hero=[pscustomobject]@{id="0x0001000$number";relation='self';x=(10*$number);y=(20*$number);movement=45;inside=$true}
        $mock.moves[$role]=@{hero=(Copy-Shape $hero);x=(10*$number+5);y=(20*$number+6)}
        $mock.worlds[$role]=[pscustomobject]@{worldSeq=(100+$number);stacks=@($hero)}
        $mock.bindings[$role]=[pscustomobject]@{Appearance=(20+$number);Instance=(80+$number)}
        $mock.completions[$role]=[Threading.Tasks.TaskCompletionSource[Net.Http.HttpResponseMessage]]::new()
    }
    $mock.client=[OfflineLobbyHttpClient]::new($mock.completions,$mock.events)
    $mock.saveFailure=$false; $mock.bindingFailure=''
}
function New-OfflineLobbyHttpClient { $mock.client }
function Get-World([string]$Role) { $mock.events.Add("world-$Role"); $mock.worlds[$Role] }
function Get-MapActionTargetBinding([string]$Role) {
    $mock.events.Add("binding-$Role")
    if($mock.bindingFailure -ceq $Role) { throw 'mock no exact native binding' }
    $mock.bindings[$Role]
}
function Save-Json([string]$Name,$Value) {
    $mock.events.Add("save-$Name")
    if($mock.saveFailure) { throw 'mock durable receipt write failed' }
    $mock.saved.Add([pscustomobject]@{name=$Name;value=(Copy-Shape $Value)})
}
function Move-Receipt($Pending,[string]$Role) {
    $i=$Pending.intents[$Role]
    [pscustomobject]@{found=$true;role=$Role;move=[pscustomobject]@{
        id=$i.id;x=$i.x;y=$i.y;appearance=$i.appearance;instance=$i.instance}}
}
function Complete-Move([string]$Role,$Receipt,[int]$Status=200,[string]$Raw='') {
    $http=[Net.Http.HttpResponseMessage]::new([Net.HttpStatusCode]$Status)
    $body=if($Raw){$Raw}else{$Receipt | ConvertTo-Json -Depth 20 -Compress}
    $http.Content=[Net.Http.StringContent]::new($body)
    $mock.completions[$Role].SetResult($http)
}

Reset-Moves
$pending=Start-LobbyMovePair $mock.moves 'test-move'
Check ($pending -is [pscustomobject] -and $pending.tasks.Count -eq 2 -and $pending.responses.Count -eq 0) 'Move return shape was polluted by incidental pipeline output'
Check (($mock.events -join ',') -ceq 'world-host,binding-host,world-join,binding-join,save-test-move-intents.json,post-host,post-join') 'Both worlds and durable claim must precede either send'
Check ($mock.client.Timeout.TotalSeconds -eq 45 -and $mock.client.Requests.Count -eq 2) 'Move sender lost bounded one-per-role dispatch'
foreach($request in $mock.client.Requests) {
    $role=$request.query.role; $i=$pending.intents[$role]
    Check (([uri]$request.uri).AbsolutePath -ceq '/api/ui/move-toward' -and $request.body -ceq '') 'Unexpected movement API or body'
    Check ($request.query.Count -eq 7 -and $request.query.id -ceq $i.id -and $request.query.x -ceq [string]$i.x -and
        $request.query.y -ceq [string]$i.y -and $request.query.appearance -ceq [string]$i.appearance -and
        $request.query.instance -ceq [string]$i.instance -and $request.query.timeoutMs -ceq '30000') 'Move query lost saved native intent identity'
    Check ($i.worldSeq -eq $mock.worlds[$role].worldSeq -and $i.source.id -ceq $i.id) 'Durable move intent lost its source observation'
}
Check (-not (Read-LobbyMoveReceipts $pending)) 'Incomplete move tasks became success'
Complete-Move host (Move-Receipt $pending host)
Check (-not (Read-LobbyMoveReceipts $pending) -and $pending.responses.Count -eq 1) 'One receipt became pair success'
Check (-not (Read-LobbyMoveReceipts $pending) -and $mock.saved.Count -eq 2) 'Completed role receipt was read/saved twice'
Complete-Move join (Move-Receipt $pending join)
Check (Read-LobbyMoveReceipts $pending) 'Two exact receipts did not complete pair'
Check ((Read-LobbyMoveReceipts $pending) -and $mock.saved.Count -eq 3 -and $mock.client.Requests.Count -eq 2) 'Receipt polling retried a native move'

foreach($mutation in @('missing','duplicate','relation','x','y','movement','inside')) {
    Reset-Moves
    switch($mutation) {
        missing { $mock.worlds.join.stacks=@() }
        duplicate { $mock.worlds.join.stacks=@($mock.worlds.join.stacks[0],$mock.worlds.join.stacks[0]) }
        relation { $mock.worlds.join.stacks[0].relation='enemy' }
        x { $mock.worlds.join.stacks[0].x++ }
        y { $mock.worlds.join.stacks[0].y++ }
        movement { $mock.worlds.join.stacks[0].movement-- }
        inside { $mock.worlds.join.stacks[0].inside=$false }
    }
    Reject { Start-LobbyMovePair $mock.moves 'reject-source' } '*source changed*'
    Check ($mock.client.Requests.Count -eq 0 -and $mock.saved.Count -eq 0) "Changed join $mutation allowed partial host dispatch"
}
Reset-Moves; $mock.bindingFailure='join'
Reject { Start-LobbyMovePair $mock.moves 'reject-binding' } '*no exact native binding*'
Check ($mock.client.Requests.Count -eq 0) 'Invalid second native owner allowed first send'
Reset-Moves; $mock.saveFailure=$true
Reject { Start-LobbyMovePair $mock.moves 'reject-claim' } '*durable receipt write failed*'
Check ($mock.client.Requests.Count -eq 0) 'Failed durable claim allowed movement'
foreach($role in @('host','join')) {
    Reset-Moves; $mock.client.FailRole=$role
    Reject { Start-LobbyMovePair $mock.moves 'ambiguous-send' } '*mock ambiguous*'
    $expected=if($role -ceq 'host'){1}else{2}
    Check ($mock.client.Requests.Count -eq $expected -and $mock.client.Disposed) 'Ambiguous move send retried or leaked client'
}
foreach($mutation in @('found-false','found-string','role','role-array','id','id-array','x','x-string','x-array','y','appearance','instance','missing-move','http','json','task')) {
    Reset-Moves
    $pending=Start-LobbyMovePair $mock.moves 'reject-receipt'
    $receipt=Move-Receipt $pending host
    switch($mutation) {
        found-false { $receipt.found=$false }
        found-string { $receipt.found='true' }
        role { $receipt.role='join' }
        role-array { $receipt.role=@('host','host') }
        id { $receipt.move.id='0x00019999' }
        id-array { $receipt.move.id=@($receipt.move.id,$receipt.move.id) }
        x { $receipt.move.x++ }
        x-string { $receipt.move.x=[string]$receipt.move.x }
        x-array { $receipt.move.x=@($receipt.move.x,$receipt.move.x) }
        y { $receipt.move.y++ }
        appearance { $receipt.move.appearance++ }
        instance { $receipt.move.instance++ }
        missing-move { $receipt.PSObject.Properties.Remove('move') }
    }
    if($mutation -ceq 'task') { $mock.completions.host.SetException([InvalidOperationException]::new('mock ambiguous task failure')) }
    elseif($mutation -ceq 'http') { Complete-Move host $receipt 409 }
    elseif($mutation -ceq 'json') { Complete-Move host $receipt 200 '{invalid-json' }
    else { Complete-Move host $receipt }
    Reject { Read-LobbyMoveReceipts $pending } '*' "move $mutation"
    Check ($pending.responses.Count -eq 0 -and $mock.client.Requests.Count -eq 2 -and $mock.saved.Count -eq 1) "Malformed $mutation receipt was accepted, persisted or retried"
}

# Actual paired EndTurn with one strict mock endpoint; no network implementation
# is loaded. Test source observations and response shapes separately.
function Reset-EndTurns {
    $mock.Clear(); $mock.events=[Collections.Generic.List[string]]::new()
    $mock.saved=[Collections.Generic.List[object]]::new(); $mock.requests=0; $mock.saveFailure=$false; $mock.transportFailure=$false
    $mock.pair=@{}; $mock.response=[pscustomobject]@{found=$true;host=$null;join=$null;dispatchSkewMs=0.25}
    foreach($role in @('host','join')) {
        $n=if($role -ceq 'host'){1}else{2}
        $ui=[pscustomobject]@{dialog='DLG_STRATEGIC';dialogReady=$true;strategicIdle=$true;dialogAppearance=(10+$n);dialogInstance=(10+$n);uiSeq=(100+$n);
            targets=@([pscustomobject]@{dialog='DLG_STRATEGIC';instance=(40+$n);widgets=@([pscustomobject]@{name='BTN_END_TURN';type='button';state=[pscustomobject]@{enabled=$true}})})}
        $mock.pair[$role]=[pscustomobject]@{ui=$ui;world=[pscustomobject]@{day=1}}
        $mock.response.$role=[pscustomobject]@{found=$true;role=$role;strategicIdle=$true;invoke=[pscustomobject]@{
            dlg='DLG_STRATEGIC';btn='BTN_END_TURN';appearance=$ui.dialogAppearance;instance=$ui.targets[0].instance}}
    }
}
function Invoke-RestMethod([string]$Method,[string]$Uri,[int]$TimeoutSec) {
    $mock.requests++; $mock.events.Add('post-endturn')
    Check ($Method -ceq 'POST' -and $TimeoutSec -eq 135 -and ([uri]$Uri).AbsolutePath -ceq '/api/ui/end-turn-pair-when-strategic-idle') 'EndTurn API or timeout drifted'
    $query=Read-Query $Uri
    Check ($query.Count -eq 8 -and $query.waitMs -ceq '120000' -and $query.timeoutMs -ceq '8000') 'EndTurn query shape changed'
    foreach($role in @('host','join')) {
        $ui=$mock.pair[$role].ui
        Check ($query["${role}appearance"] -ceq [string]$ui.dialogAppearance -and
            $query["${role}instance"] -ceq [string]$ui.targets[0].instance -and
            $query["${role}ui"] -ceq [string]$ui.uiSeq) 'EndTurn query lost a saved owner/watermark'
    }
    if($mock.transportFailure) { throw 'mock ambiguous EndTurn transport' }
    return $mock.response
}
Reset-EndTurns
$result=Invoke-LobbyEndTurnPair $mock.pair 1
Check ($result -is [pscustomobject] -and $result.found -ceq $true -and $mock.requests -eq 1) 'EndTurn return shape or one-shot request changed'
Check (($mock.events -join ',') -ceq 'save-day-1-endturn-intents.json,post-endturn,save-day-1-endturn-receipt.json') 'EndTurn mutation preceded its durable claim'
foreach($mutation in @('duplicate-owner','missing-owner','duplicate-button','disabled','busy','unready','ready-string','idle-string','enabled-string','appearance-zero','appearance-alias','owner-zero','ui-zero','owner-string','ui-fraction')) {
    Reset-EndTurns
    $ui=$mock.pair.join.ui
    switch($mutation) {
        duplicate-owner { $ui.targets=@($ui.targets[0],$ui.targets[0]) }
        missing-owner { $ui.targets=@() }
        duplicate-button { $ui.targets[0].widgets=@($ui.targets[0].widgets[0],$ui.targets[0].widgets[0]) }
        disabled { $ui.targets[0].widgets[0].state.enabled=$false }
        busy { $ui.strategicIdle=$false }
        unready { $ui.dialogReady=$false }
        ready-string { $ui.dialogReady='true' }
        idle-string { $ui.strategicIdle='true' }
        enabled-string { $ui.targets[0].widgets[0].state.enabled='true' }
        appearance-zero { $ui.dialogAppearance=0 }
        appearance-alias { $ui.dialogInstance++ }
        owner-zero { $ui.targets[0].instance=0 }
        ui-zero { $ui.uiSeq=0 }
        owner-string { $ui.targets[0].instance=[string]$ui.targets[0].instance }
        ui-fraction { $ui.uiSeq=101.5 }
    }
    Reject { Invoke-LobbyEndTurnPair $mock.pair 1 } '*' "EndTurn source $mutation"
    Check ($mock.requests -eq 0 -and $mock.saved.Count -eq 0) "Invalid EndTurn $mutation reached native mutation"
}
foreach($mutation in @('found-false','found-string','role-found-string','role-idle-string','role','role-array','dialog','dialog-array','button','button-array','appearance','appearance-string','appearance-array','owner','missing-role','transport')) {
    Reset-EndTurns
    switch($mutation) {
        found-false { $mock.response.found=$false }
        found-string { $mock.response.found='true' }
        role-found-string { $mock.response.join.found='true' }
        role-idle-string { $mock.response.join.strategicIdle='true' }
        role { $mock.response.join.role='host' }
        role-array { $mock.response.join.role=@('join','join') }
        dialog { $mock.response.join.invoke.dlg='DLG_MESSAGE_BOX' }
        dialog-array { $mock.response.join.invoke.dlg=@('DLG_STRATEGIC','DLG_STRATEGIC') }
        button { $mock.response.join.invoke.btn='BTN_OK' }
        button-array { $mock.response.join.invoke.btn=@('BTN_END_TURN','BTN_END_TURN') }
        appearance { $mock.response.join.invoke.appearance++ }
        appearance-string { $mock.response.join.invoke.appearance=[string]$mock.response.join.invoke.appearance }
        appearance-array { $mock.response.join.invoke.appearance=@($mock.response.join.invoke.appearance,$mock.response.join.invoke.appearance) }
        owner { $mock.response.join.invoke.instance++ }
        missing-role { $mock.response.PSObject.Properties.Remove('join') }
        transport { $mock.transportFailure=$true }
    }
    Reject { Invoke-LobbyEndTurnPair $mock.pair 1 } '*' "EndTurn receipt $mutation"
    Check ($mock.requests -eq 1) "Malformed EndTurn $mutation retried the paired mutation"
}

# Execute the real high-level workflow against in-memory oracles. A failed
# earlier proof must prevent later mutations, and day 3 must not require both
# stock roles to be independently idle after the natural merge.
& {
    $flow=@{}
    function Reset-Flow {
        $flow.Clear(); $flow.events=[Collections.Generic.List[string]]::new(); $flow.waits=[Collections.Generic.List[int]]::new()
        $flow.turns=[Collections.Generic.List[int]]::new(); $flow.fault=''; $flow.planCount=0; $flow.saved=[Collections.Generic.List[string]]::new()
        $script:GameplayProof=$null; $script:Now=[DateTime]::Parse('2026-09-27T00:00:00Z')
        $script:logEvidence=@{host=@{stockTurnsReleased=$true};join=@{stockTurnsReleased=$true}}
        $script:ClientLogs=@{host='offline-host';join='offline-join'}
    }
    function Import-LobbyBattleObservers { $flow.events.Add('import') }
    function Wait-LobbyGameplayIdle([int]$Day) {
        $flow.waits.Add($Day); $flow.events.Add("idle-$Day")
        return @{host=@{world=@{day=$Day}};join=@{world=@{day=$Day}}}
    }
    function Get-LobbyBattlePlan($HostWorld,$JoinWorld) {
        $flow.planCount++; $flow.events.Add("plan-$($flow.planCount)")
        return @{revision=$flow.planCount}
    }
    function Save-Json([string]$Name,$Value) { $flow.saved.Add($Name) }
    function Invoke-LobbyCapitalExits($Plan) {
        Check ($Plan.revision -eq 1) 'Capital exit used the wrong world plan'
        $flow.events.Add('exit'); if($flow.fault -ceq 'exit'){throw 'mock exit failed'}
    }
    function Invoke-LobbyConcurrentBattles($Plan) {
        Check ($Plan.revision -eq 2) 'Battles reused the pre-exit plan'
        $flow.events.Add('battles'); if($flow.fault -ceq 'battles'){throw 'mock overlap failed'}
        return @{overlap='proved';native='proved'}
    }
    function Get-LobbyBattleOutcome($Plan,$HostWorld,$JoinWorld) {
        Check ($Plan.revision -eq 2) 'Outcome changed the attacked target IDs'
        $flow.events.Add('outcome'); if($flow.fault -ceq 'outcome'){throw 'mock battle outcome failed'}
        return @{verified=$true}
    }
    function Invoke-LobbyEndTurnPair($Pair,[int]$Day) { $flow.turns.Add($Day); $flow.events.Add("end-$Day"); return @{day=$Day} }
    function Get-PairUtcNow { $script:Now }
    function Start-Sleep([int]$Milliseconds) { $script:Now=$script:Now.AddSeconds(121) }
    function Get-LobbyGameplaySnapshot([string]$Stage) {
        $flow.events.Add('merge-snapshot'); return @{host=@{world=@{day=$ExpectedMergeDay}};join=@{world=@{day=$ExpectedMergeDay}}}
    }
    function Read-ClientLogLines([string]$Path) { "offline-line-$Path" }
    function Get-LobbyMergeProof($HostLines,$JoinLines,$HostWorld,$JoinWorld,[int]$ExpectedDay) {
        $flow.events.Add('merge-proof')
        Check ($ExpectedDay -eq $ExpectedMergeDay -and $HostLines -ceq 'offline-line-offline-host' -and $JoinLines -ceq 'offline-line-offline-join') 'Merge oracle lost configured day or role logs'
        if($flow.fault -ceq 'merge'){return @{passed=$false;day=$ExpectedDay}}
        return @{passed=$true;day=$ExpectedDay}
    }
    foreach($day in @(2,3)) {
        Reset-Flow; $ExpectedMergeDay=$day
        Invoke-LobbyConcurrentBattleMerge
        Check (($flow.turns -join ',') -ceq $(if($day -eq 2){'1'}else{'1,2'})) 'Wrong independent day EndTurn sequence'
        Check (($flow.waits -join ',') -ceq $(if($day -eq 2){'1,1,1'}else{'1,1,1,2'})) 'Workflow waited for independent stock roles or skipped the intermediate day'
        Check ($script:GameplayProof.merge.day -eq $day -and $script:GameplayProof.endTurns.Count -eq ($day-1) -and
            $script:GameplayProof.campaign18 -ceq $false) 'Workflow overstated or omitted final proof'
        Check ($flow.saved[-1] -ceq 'concurrent-battle-merge-proof.json') 'Final proof was not persisted after all oracles'
    }
    foreach($fault in @('exit','battles','outcome')) {
        Reset-Flow; $ExpectedMergeDay=3; $flow.fault=$fault
        Reject { Invoke-LobbyConcurrentBattleMerge } '*mock*failed*'
        Check ($flow.turns.Count -eq 0 -and $null -eq $script:GameplayProof -and -not $flow.saved.Contains('concurrent-battle-merge-proof.json')) "Failed $fault allowed EndTurn or success proof"
    }
    foreach($fault in @('merge','host-release','join-release')) {
        Reset-Flow; $ExpectedMergeDay=3; $flow.fault=$fault
        if($fault -ceq 'host-release'){$script:logEvidence.host.stockTurnsReleased=$false}
        if($fault -ceq 'join-release'){$script:logEvidence.join.stockTurnsReleased=$false}
        Reject { Invoke-LobbyConcurrentBattleMerge } '*Natural merge*not proved*'
        Check (($flow.turns -join ',') -ceq '1,2' -and $null -eq $script:GameplayProof) "Failed $fault retried a turn or emitted success"
        if($fault -cne 'merge') { Check (-not $flow.events.Contains('merge-proof')) 'Single-role Stock release reached the paired merge oracle' }
    }
}
# Import the actual pure native receipt observers and the actual A/B live-battle
# adapter. The shared campaign's legacy A records must remain valid, while B
# requires an explicit matching scalar layout in its auto-battle proof.
& {
    $observerPath=Join-Path $repo 'tools/test/_simturns_gameplay.ps1'
    $observerAst=[Management.Automation.Language.Parser]::ParseFile($observerPath,[ref]$tokens,[ref]$errors)
    if($errors.Count) { throw ($errors | Out-String) }
    foreach($name in @('Read-PrearmedAutoBattleProof','ConvertFrom-ScriptedBattleCloseMarker','Assert-ScriptedBattleCloseProof',
        'Assert-LegacyBattleUpSnapshot','Test-LegacyBattleCloseCommitted')) {
        $nodes=@($observerAst.FindAll({param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -ceq $name},$false))
        Check ($nodes.Count -eq 1) "Native observer $name is not unique"
        . ([scriptblock]::Create($nodes[0].Extent.Text))
    }
    foreach($name in @('Get-LobbyGameplayProperty','Get-LobbyGameplayIntegerProperty','Get-LobbyLiveBattleProof','Assert-LobbyBattleOverlap','Assert-LobbyNativeBattleOverlap')) {
        $nodes=@($policyAst.FindAll({param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -ceq $name},$false))
        Check ($nodes.Count -eq 1) "Pure battle policy $name is not unique"
        . ([scriptblock]::Create($nodes[0].Extent.Text))
    }
    foreach($name in @('Find-LobbyLiveBattleProof','Invoke-LobbyConcurrentBattles')) {
        . ([scriptblock]::Create($functions[$name].Extent.Text))
    }
    function Write-Step([string]$Text) { }
    function Battle-Ui([string]$Dialog,[string]$Role='host') {
        $n=if($Role -ceq 'host'){1}else{2}
        [pscustomobject]@{dialog=$Dialog;dialogReady=$true;dialogAppearance=(10+$n);dialogInstance=(10+$n);uiSeq=(100+$n);
            targets=@([pscustomobject]@{dialog=$Dialog;instance=(40+$n);widgets=@([pscustomobject]@{name='BTN_DEFEND';type='button'})})}
    }
    function Auto-Proof([string]$Dialog,[string]$Role='host',[bool]$IncludeDialog=$true) {
        $ui=Battle-Ui $Dialog $Role
        $proof=[pscustomobject]@{schema=1;mode='preboot-first-battle';role=$Role;succeeded=$true;
            appearance=$ui.dialogAppearance;owner=$ui.targets[0].instance;bindAgeMs=2500;callbackCount=1;
            functorVftable=0x006F45D4;dispatchFunction=0x00644150;memberFunction=0x00635509;thisAdjustor=0;
            controllerGateBefore=0;kickStateBefore=0;kickStateAfter=1;sideSelector=1;
            flag38Before=0;flag38After=1;flag39Before=0;flag39After=0}
        if($IncludeDialog) { $proof | Add-Member -NotePropertyName dialog -NotePropertyValue $Dialog }
        return $proof
    }
    function Auto-Line($Proof) { '[testdrv][auto-battle-proof] '+($Proof | ConvertTo-Json -Depth 10 -Compress) }
    function Close-Lines([string]$Dialog,[string]$Role='host',[long]$ObservedTick=1000) {
        $ui=Battle-Ui $Dialog $Role
        $identity="role=$Role dialog=$Dialog appearance=$($ui.dialogAppearance) owner=$($ui.targets[0].instance) button=BTN_CLOSE"
        @("[testdrv][scripted-popup] OBSERVED $identity tick=$ObservedTick",
          "[testdrv][scripted-popup] CLAIMED $identity bindAgeMs=300 tick=$($ObservedTick+300)",
          "[testdrv][scripted-popup] COMMITTED $identity tick=$($ObservedTick+310)")
    }
    function Close-State([string]$Dialog,[string]$Role='host',[bool]$IncludeDialog=$true) {
        $ui=Battle-Ui $Dialog $Role
        $state=[pscustomobject]@{Role=$Role;LiveBattleAppearance=$ui.dialogAppearance;LiveBattleOwner=$ui.targets[0].instance}
        if($IncludeDialog) { $state | Add-Member -NotePropertyName LiveBattleDialog -NotePropertyValue $Dialog }
        return $state
    }
    foreach($dialog in @('DLG_BATTLE_A','DLG_BATTLE_B')) {
        $ui=Battle-Ui $dialog; $closeLines=Close-Lines $dialog
        [string[]]$blankLog=@('', 'unrelated log line', (Auto-Line (Auto-Proof $dialog)), '',
            $closeLines[0], '', $closeLines[1], '  ', $closeLines[2], '')
        $baseline=[pscustomobject]@{role='host';lineCount=2}
        $auto=Read-PrearmedAutoBattleProof -Role host -Lines $blankLog -BattleUi $ui
        Check ($auto.found -and $auto.kick.dialog -ceq $dialog) "Blank-containing $dialog log rejected valid auto proof"
        $close=Assert-ScriptedBattleCloseProof -State (Close-State $dialog) -Baseline $baseline -Lines $blankLog
        Check ($close.callbackCount -eq 1 -and $close.owner -eq 41) "Blank-containing $dialog log rejected valid close chain"
        $legacyState=[pscustomobject]@{Role='host';BattleUi=$ui}
        Check (Assert-LegacyBattleUpSnapshot -State $legacyState -Baseline $baseline -Lines $blankLog).found "Legacy battle-up wrapper rejected blank-containing $dialog log"
        if($dialog -ceq 'DLG_BATTLE_A') {
            Check (Test-LegacyBattleCloseCommitted -Role host -Baseline $baseline -Lines $blankLog) 'Legacy close wrapper rejected blank-containing A log'
        }
        $badLog=@($blankLog); $badLog[6]=$badLog[6].Replace('owner=41','owner=42')
        Reject { Assert-ScriptedBattleCloseProof (Close-State $dialog) $baseline $badLog } '*identity changed*' "Blank lines hid invalid $dialog close owner"
    }
    foreach($kind in @('empty-collection','one-empty-line','blank-only')) {
        [string[]]$emptyLog=switch($kind) {
            empty-collection { @() }
            one-empty-line { ,'' }
            blank-only { @('', '  ', '', "`t") }
        }
        if($null -eq $emptyLog) { $emptyLog=[string[]]@() }
        $baseline=[pscustomobject]@{role='host';lineCount=0}
        Check ($null -eq (Read-PrearmedAutoBattleProof host $emptyLog (Battle-Ui DLG_BATTLE_B))) "$kind log fabricated auto proof"
        Reject { Assert-ScriptedBattleCloseProof (Close-State DLG_BATTLE_B) $baseline $emptyLog } '*exact*chain*' "$kind close log"
        Reject { Assert-LegacyBattleUpSnapshot ([pscustomobject]@{Role='host';BattleUi=(Battle-Ui DLG_BATTLE_A)}) $baseline $emptyLog } '*no exact live-battle auto proof*' "$kind legacy up log"
        Check (-not (Test-LegacyBattleCloseCommitted host $baseline $emptyLog)) "$kind legacy close log fabricated committed marker"
    }
    Reject { Read-PrearmedAutoBattleProof host @('', '[testdrv][auto-battle-proof] {invalid', '') (Battle-Ui DLG_BATTLE_B) } '*not complete JSON*' 'Blank lines hid malformed JSON'
    $blankChain=@('', (Close-Lines DLG_BATTLE_B))
    Reject { Assert-ScriptedBattleCloseProof (Close-State DLG_BATTLE_B) ([pscustomobject]@{role='host';lineCount=99}) $blankChain } '*regressed below*' 'Blank tolerance weakened log baseline'

    if($ReplayArtifactDirectory) {
        $replayPath=[IO.Path]::GetFullPath($ReplayArtifactDirectory)
        $artifactRoot=[IO.Path]::GetFullPath((Join-Path $repo 'artifacts'))+[IO.Path]::DirectorySeparatorChar
        Check ($replayPath.StartsWith($artifactRoot,[StringComparison]::OrdinalIgnoreCase)) 'Replay must read an explicitly selected finished run under this repository artifacts directory'
        foreach($role in @('host','join')) {
            $logFile=Join-Path $replayPath "$role.mss32.log"
            $uiFile=Join-Path $replayPath "$role-first-live-battle.json"
            Check ([IO.File]::Exists($logFile) -and [IO.File]::Exists($uiFile)) "Finished replay omitted $role log or immutable battle snapshot"
            $logHash=(Get-FileHash -LiteralPath $logFile -Algorithm SHA256).Hash
            [string[]]$lines=[IO.File]::ReadAllLines($logFile)
            $ui=[IO.File]::ReadAllText($uiFile) | ConvertFrom-Json
            Check (@($lines | Where-Object { $_ -ceq '' }).Count -gt 0) "$role replay does not reproduce the blank-line regression"
            $live=Get-LobbyLiveBattleProof -Ui $ui -Role $role
            $auto=Read-PrearmedAutoBattleProof -Role $role -Lines $lines -BattleUi $ui
            $state=[pscustomobject]@{Role=$role;LiveBattleDialog=$live.dialog;LiveBattleAppearance=$live.appearance;LiveBattleOwner=$live.owner}
            $close=Assert-ScriptedBattleCloseProof -State $state -Baseline ([pscustomobject]@{role=$role;lineCount=0}) -Lines $lines
            Check ($auto.found -and $auto.kick.dialog -ceq 'DLG_BATTLE_B' -and $auto.kick.owner -eq $live.owner) "$role real layout-B auto callback failed replay"
            Check ($close.callbackCount -eq 1 -and $close.owner -eq $live.owner -and $close.appearance -eq $live.appearance) "$role real layout-B close chain failed replay"
            Check ((Get-FileHash -LiteralPath $logFile -Algorithm SHA256).Hash -ceq $logHash) "$role replay changed its source log"
        }
    }
    foreach($dialog in @('DLG_BATTLE_A','DLG_BATTLE_B')) {
        $ui=Battle-Ui $dialog
        $record=Read-PrearmedAutoBattleProof -Role host -Lines @(Auto-Line (Auto-Proof $dialog)) -BattleUi $ui
        Check ($record -is [pscustomobject] -and $record.found -and $record.kick.dialog -ceq $dialog) "Exact $dialog auto proof was rejected or return shape polluted"
        $live=Find-LobbyLiveBattleProof $ui host
        Check ($live.dialog -ceq $dialog -and $live.owner -eq 41 -and $live.appearance -eq 11) "Live $dialog adapter lost layout or owner"
        $resultUi=Battle-Ui $dialog
        $resultUi.targets[0].widgets+=@([pscustomobject]@{name='BTN_CLOSE';type='button'})
        Check ($null -eq (Find-LobbyLiveBattleProof $resultUi host)) "Result layout $dialog incorrectly proved live overlap"
        $ui.dialogReady=$false
        Check ($null -eq (Find-LobbyLiveBattleProof $ui host)) "Unready $dialog became live battle evidence"
        $lines=Close-Lines $dialog
        foreach($index in 0..2) {
            $marker=ConvertFrom-ScriptedBattleCloseMarker -Line $lines[$index] -Role host -Dialog $dialog
            Check ($marker.kind -ceq @('OBSERVED','CLAIMED','COMMITTED')[$index] -and $marker.owner -eq 41) "Exact $dialog close marker $index failed"
        }
        $close=Assert-ScriptedBattleCloseProof -State (Close-State $dialog) -Baseline ([pscustomobject]@{role='host';lineCount=3}) -Lines @($lines+$lines)
        Check ($close.callbackCount -eq 1 -and $close.bindAgeMs -eq 300 -and $close.owner -eq 41) "Close $dialog proof did not honor its pre-battle boundary"
        $other=if($dialog -ceq 'DLG_BATTLE_A'){'DLG_BATTLE_B'}else{'DLG_BATTLE_A'}
        Reject { Read-PrearmedAutoBattleProof host @(Auto-Line (Auto-Proof $other)) (Battle-Ui $dialog) } '*observed battle layout*' "auto $other cannot prove $dialog"
        Check ($null -eq (ConvertFrom-ScriptedBattleCloseMarker -Line $lines[0] -Role host -Dialog $other)) 'Close parser crossed layouts'
        Reject { Assert-ScriptedBattleCloseProof (Close-State $other) ([pscustomobject]@{role='host';lineCount=0}) $lines } '*exact*chain*' "close $dialog cannot prove $other"
        foreach($shape in @('array','null','number','lowercase')) {
            $proof=Auto-Proof $dialog
            switch($shape) {
                array { $proof.dialog=@($dialog,$dialog) }
                null { $proof.dialog=$null }
                number { $proof.dialog=1 }
                lowercase { $proof.dialog=$dialog.ToLowerInvariant() }
            }
            Reject { Read-PrearmedAutoBattleProof host @(Auto-Line $proof) (Battle-Ui $dialog) } '*scalar string matching*' "$dialog $shape"
        }
    }
    $legacy=Read-PrearmedAutoBattleProof host @(Auto-Line (Auto-Proof DLG_BATTLE_A host $false)) (Battle-Ui DLG_BATTLE_A)
    Check ($legacy.found -and -not $legacy.kick.PSObject.Properties['dialog']) 'Legacy A auto proof compatibility was lost'
    Check (-not $legacy.kick.PSObject.Properties['committedTick64']) 'Legacy auto proof fabricated a native timestamp'
    foreach($tick in @([long]1,[long]5000000000,[long]::MaxValue)) {
        $proof=Auto-Proof DLG_BATTLE_B
        $proof | Add-Member -NotePropertyName committedTick64 -NotePropertyValue $tick
        $record=Read-PrearmedAutoBattleProof host @(Auto-Line $proof) (Battle-Ui DLG_BATTLE_B)
        Check ($record.kick.committedTick64 -is [long] -and $record.kick.committedTick64 -eq $tick) 'Optional native tick was narrowed, omitted or changed'
    }
    foreach($shape in @('zero','negative','string','fraction','array','null','boolean')) {
        $proof=Auto-Proof DLG_BATTLE_B
        $tick=switch($shape) {
            zero { 0 }; negative { -1 }; string { '5000000000' }; fraction { 5000000000.5 }
            array { ,@(5000000000,5000000000) }; null { $null }; boolean { $true }
        }
        $proof | Add-Member -NotePropertyName committedTick64 -NotePropertyValue $tick
        Reject { Read-PrearmedAutoBattleProof host @(Auto-Line $proof) (Battle-Ui DLG_BATTLE_B) } '*committedTick64 must be a positive JSON integer*' "native tick $shape"
    }
    Reject { Read-PrearmedAutoBattleProof host @(Auto-Line (Auto-Proof DLG_BATTLE_B host $false)) (Battle-Ui DLG_BATTLE_B) } '*omitted exact layout B*'
    Check ($null -eq (Read-PrearmedAutoBattleProof host @() (Battle-Ui DLG_BATTLE_B))) 'Missing native B auto proof fabricated success'
    $line=Auto-Line (Auto-Proof DLG_BATTLE_B)
    Reject { Read-PrearmedAutoBattleProof host @($line,$line) (Battle-Ui DLG_BATTLE_B) } '*expected exactly one*'
    Reject { Read-PrearmedAutoBattleProof host @($line) (Battle-Ui DLG_BATTLE_C) } '*Unsupported battle layout*'
    foreach($field in @('owner','appearance','callbackCount','memberFunction')) {
        $proof=Auto-Proof DLG_BATTLE_B; $proof.$field++
        Reject { Read-PrearmedAutoBattleProof host @(Auto-Line $proof) (Battle-Ui DLG_BATTLE_B) } '*callback invariant*' "B retained $field guard"
    }
    $linesA=Close-Lines DLG_BATTLE_A; $linesB=Close-Lines DLG_BATTLE_B
    Check ((ConvertFrom-ScriptedBattleCloseMarker $linesA[0] host).kind -ceq 'OBSERVED') 'Default close parser lost legacy A compatibility'
    Check ($null -eq (ConvertFrom-ScriptedBattleCloseMarker $linesB[0] host)) 'Default A parser silently accepted B'
    Check ((Assert-ScriptedBattleCloseProof (Close-State DLG_BATTLE_A host $false) ([pscustomobject]@{role='host';lineCount=0}) $linesA).callbackCount -eq 1) 'Legacy close state lost its A default'
    Reject { Assert-ScriptedBattleCloseProof (Close-State DLG_BATTLE_B host $false) ([pscustomobject]@{role='host';lineCount=0}) $linesB } '*exact*chain*'
    foreach($mutation in @('mixed-layout','owner','appearance','role','duplicate','missing','age','tick-order','malformed')) {
        $lines=@(Close-Lines DLG_BATTLE_B)
        switch($mutation) {
            mixed-layout { $lines[1]=$lines[1].Replace('DLG_BATTLE_B','DLG_BATTLE_A') }
            owner { $lines[1]=$lines[1].Replace('owner=41','owner=42') }
            appearance { $lines[1]=$lines[1].Replace('appearance=11','appearance=12') }
            role { $lines[1]=$lines[1].Replace('role=host','role=join') }
            duplicate { $lines+=@($lines[2]) }
            missing { $lines=@($lines[0],$lines[2]) }
            age { $lines[1]=$lines[1].Replace('bindAgeMs=300','bindAgeMs=299') }
            tick-order { $lines[2]=$lines[2].Replace('tick=1310','tick=1299') }
            malformed { $lines[1]+=' unexpected' }
        }
        Reject { Assert-ScriptedBattleCloseProof (Close-State DLG_BATTLE_B) ([pscustomobject]@{role='host';lineCount=0}) $lines } '*' "B close $mutation"
    }

    # Execute the real battle loop with A on host, B on join. This ensures its
    # observed layout is forwarded into both actual receipt parsers, not merely
    # that those parsers work when a test supplies the correct layout directly.
    $battle=@{}
    function Reset-BattleMock {
        $battle.Clear()
        $battle.iteration=0; $battle.requests=0; $battle.reads=0; $battle.uiReads=0
        $battle.saved=[Collections.Generic.List[string]]::new()
        $battle.now=[DateTimeOffset]::Parse('2026-09-27T00:00:00Z')
        $battle.client=[pscustomobject]@{disposed=$false}
        $battle.client | Add-Member -MemberType ScriptMethod -Name Dispose -Value { $this.disposed=$true }
        $battle.tickMode='valid'; $battle.suppressBracket=$false; $battle.rejectBracket=$false
        $battle.starts=@{host=[long]5000000200;join=[long]5000000400}
        $battle.ends=@{host=[long]5000001000;join=[long]5000001000}
    }
    Reset-BattleMock
    $script:ClientLogs=@{host='host';join='join'}
    $script:logEvidence=@{host=@{bootstrapOperational=$true};join=@{bootstrapOperational=$true}}
    function Assert-PairProgress { }
    function Start-LobbyMovePair($Moves,[string]$Stage) {
        $battle.requests++
        Check ($Stage -ceq 'parallel-attacks' -and $Moves.Count -eq 2) 'Battle loop changed the sole paired attack'
        return [pscustomobject]@{client=$battle.client}
    }
    function Read-LobbyMoveReceipts($Pending) { $battle.reads++; return $true }
    function Get-PairUtcNow { $battle.now=$battle.now.AddMilliseconds(1); return $battle.now }
    function Start-Sleep([int]$Milliseconds) { $battle.iteration++ }
    function Get-GameUiSnapshot([string]$Role) {
        $battle.uiReads++
        if($battle.suppressBracket -and $battle.uiReads -eq 1) { return [pscustomobject]@{dialog='DLG_STRATEGIC'} }
        if($battle.iteration -eq 0) {
            $ui=Battle-Ui $(if($Role -ceq 'host'){'DLG_BATTLE_A'}else{'DLG_BATTLE_B'}) $Role
            if($battle.rejectBracket -and $battle.uiReads -eq 1) { $ui.targets[0].instance+=10 }
            return $ui
        }
        return [pscustomobject]@{dialog='DLG_STRATEGIC'}
    }
    function Test-StartupRoleAccepted($Ui,[bool]$Bootstrap) { return $Bootstrap -and $Ui.dialog -ceq 'DLG_STRATEGIC' }
    function Save-Json([string]$Name,$Value) { $battle.saved.Add($Name) }
    function Read-ClientLogLines([string]$Role) {
        if($battle.iteration -eq 0) { return @('', 'before battle', '') }
        $dialog=if($Role -ceq 'host'){'DLG_BATTLE_A'}else{'DLG_BATTLE_B'}
        $proof=Auto-Proof $dialog $Role
        if($battle.tickMode -cne "missing-$Role") {
            $tick=$battle.starts[$Role]
            if($Role -ceq 'join') {
                switch($battle.tickMode) {
                    zero { $tick=0 }
                    string { $tick=[string]$tick }
                    fraction { $tick=[double]$tick+0.5 }
                    array { $tick=@($tick,$tick) }
                }
            }
            $proof | Add-Member -NotePropertyName committedTick64 -NotePropertyValue $tick
        }
        return @('', 'before battle', '', (Auto-Line $proof), '') + @(Close-Lines $dialog $Role $battle.ends[$Role]) + @('')
    }
    $plan=@{host=@{hero=@{id='0x00010001'};target=@{x=1;y=2}};join=@{hero=@{id='0x00010002'};target=@{x=3;y=4}}}
    $result=Invoke-LobbyConcurrentBattles $plan
    Check ($result -is [hashtable] -and $result.overlap.passed -and $result.overlap.provenOverlapMs -eq 600 -and
        $result.native.join.autoBattle.kick.dialog -ceq 'DLG_BATTLE_B') 'Actual mixed-layout battle loop failed to retain B proof or exact native interval'
    Check ($result.native.join.close.owner -eq 42 -and $result.native.host.close.owner -eq 41) 'Battle loop close receipts lost role/layout owners'
    Check ($battle.requests -eq 1 -and $battle.reads -eq 2 -and $battle.client.disposed) 'Mixed-layout battle observation retried an attack or leaked its client'
    Check ($result.observationBracket.passed -and $battle.saved.Contains('concurrent-live-battle-overlap.json') -and
        $battle.saved[-2] -ceq 'concurrent-battles-native.json' -and $battle.saved[-1] -ceq 'concurrent-native-battle-overlap.json') 'Native interval proof was not the final battle acceptance gate'
    Reset-BattleMock; $battle.suppressBracket=$true
    $result=Invoke-LobbyConcurrentBattles $plan
    Check ($result.overlap.passed -and $null -eq $result.observationBracket -and
        $battle.saved[-1] -ceq 'concurrent-native-battle-overlap.json') 'Cache bracket incorrectly replaced or gated exact native interval proof'
    Reset-BattleMock; $battle.rejectBracket=$true
    $result=Invoke-LobbyConcurrentBattles $plan
    Check ($result.overlap.passed -and $null -eq $result.observationBracket -and
        $result.observationBracketRejection.passed -ceq $false -and
        $result.observationBracketRejection.error -like '*changed identity*') 'Rejected cache bracket replaced valid native proof or fabricated observation success'
    Check ($battle.saved.Contains('concurrent-live-battle-bracket-rejected.json') -and
        -not $battle.saved.Contains('concurrent-live-battle-overlap.json') -and $battle.requests -eq 1) 'Supplemental bracket failure was not recorded or retried the attack'
    foreach($failure in @('missing-host','missing-join','zero','string','fraction','array','start-after-end','adjacent','disjoint')) {
        Reset-BattleMock; $battle.tickMode=$failure
        switch($failure) {
            start-after-end { $battle.starts.join=$battle.ends.join+1 }
            adjacent { $battle.ends.host=$battle.starts.join }
            disjoint { $battle.ends.host=$battle.starts.join-1 }
        }
        Reject { Invoke-LobbyConcurrentBattles $plan } '*' "native timing $failure"
        Check ($battle.requests -eq 1 -and $battle.client.disposed -and
            -not $battle.saved.Contains('concurrent-native-battle-overlap.json')) "Invalid native timing $failure retried or persisted battle success"
    }
}
"PASS: $script:checks offline lobby gameplay-run checks; no network, processes, file writes or game callbacks"
