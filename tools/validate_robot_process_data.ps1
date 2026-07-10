param(
  [string]$RobotDataDir = (Join-Path $PSScriptRoot '..\data\robot')
)

$ErrorActionPreference = 'Stop'

function Read-CsvStrict {
  param(
    [Parameter(Mandatory = $true)][string]$Path,
    [int]$MinRows = 1
  )
  if (!(Test-Path -LiteralPath $Path)) {
    throw "Missing CSV: $Path"
  }
  $rows = @(Import-Csv -LiteralPath $Path)
  if ($rows.Count -lt $MinRows) {
    throw "CSV has $($rows.Count) rows, expected at least $MinRows`: $Path"
  }
  return $rows
}

$manifestPath = Join-Path $RobotDataDir 'mechanics_manifest.json'
if (!(Test-Path -LiteralPath $manifestPath)) {
  throw "Missing manifest: $manifestPath"
}
$manifest = Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json

$actuators = Read-CsvStrict (Join-Path $RobotDataDir 'atar_m_series_actuator_specs.csv') 3
$axisMap = Read-CsvStrict (Join-Path $RobotDataDir 'prr_scara_axis_map.csv') 3
$modes = Read-CsvStrict (Join-Path $RobotDataDir 'robot_process_feed_modes.csv') 1
$templates = Read-CsvStrict (Join-Path $RobotDataDir 'robot_process_recipe_templates.csv') 1
$interlocks = Read-CsvStrict (Join-Path $RobotDataDir 'robot_process_interlocks.csv') 1
$validation = Read-CsvStrict (Join-Path $RobotDataDir 'robot_process_validation_matrix.csv') 1
$sources = Read-CsvStrict (Join-Path $RobotDataDir 'robot_data_sources.csv') 1
$geometry = Read-CsvStrict (Join-Path $RobotDataDir 'prr_scara_geometry_seed.csv') 1
$canMap = Read-CsvStrict (Join-Path $RobotDataDir 'canopen_actuator_bus_map.csv') 3

$selected = @($actuators | Where-Object { $_.selected_for_robot -eq 'yes' })
if ($selected.Count -ne 3) {
  throw "Expected exactly 3 selected actuators, found $($selected.Count)"
}

$selectedModels = @{}
foreach ($row in $selected) { $selectedModels[$row.model] = $true }
foreach ($axis in $axisMap) {
  if (!$selectedModels.ContainsKey($axis.actuator_model)) {
    throw "Axis $($axis.axis_id) references actuator_model not selected in atar_m_series_actuator_specs.csv: $($axis.actuator_model)"
  }
}

$axisIds = @{}
foreach ($axis in $axisMap) { $axisIds[$axis.axis_id] = $true }
foreach ($node in $canMap) {
  if (!$axisIds.ContainsKey($node.axis_id)) {
    throw "CAN node $($node.node_id) references unknown axis_id: $($node.axis_id)"
  }
}

$modeIds = @{}
foreach ($mode in $modes) { $modeIds[$mode.mode_id] = $true }
foreach ($template in $templates) {
  if (!$modeIds.ContainsKey($template.mode_id)) {
    throw "Template $($template.template_id) references unknown mode_id: $($template.mode_id)"
  }
}

$requiredManifestKeys = @(
  'atar_m_series_actuator_specs',
  'prr_scara_axis_map',
  'prr_scara_geometry_seed',
  'canopen_actuator_bus_map',
  'robot_process_feed_modes',
  'robot_process_recipe_templates',
  'robot_process_interlocks',
  'robot_process_validation_matrix',
  'robot_data_sources'
)
foreach ($key in $requiredManifestKeys) {
  if (-not ($manifest.files.PSObject.Properties.Name -contains $key)) {
    throw "mechanics_manifest.json missing files.$key"
  }
}

Write-Host "robot process data validation: OK"
Write-Host "selected actuators: $($selected.model -join ', ')"
Write-Host "prr axes: $($axisMap.axis_id -join ', ')"
Write-Host "process modes: $($modes.Count), templates: $($templates.Count), interlocks: $($interlocks.Count)"
Write-Host "validation gates: $($validation.Count), sources: $($sources.Count), geometry rows: $($geometry.Count)"
