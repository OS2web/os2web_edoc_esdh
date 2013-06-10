<?php

/**
 * @file
 * This file describes the MM plugin as implemented by eDoc
 */

/**
 * Plugin definition with callbacks
 */
$plugin = array(
  'description' => t('eDoc ESDH MM Backend'),
  'version' => 1,
  'test' => 'os2web_edoc_esdh_mm_test',
  'get_import_list' => 'os2web_edoc_esdh_mm_get_import_list',
  'import_meeting' => 'os2web_edoc_esdh_mm_import_meeting',
  'post_import_process' => '_os2web_edoc_esdh_mm_order_addenums',
);

/**
 * For plugin testing purposes.
 */
function os2web_edoc_esdh_mm_test($stuff) {
  return 'Test successful. Recieved: ' . $stuff;
}

/**
 * Returns a list of meeting_ids for import. In our case a list of uri's of xml files.
 *
 * @return array
 *   List of URIs
 */
function os2web_edoc_esdh_mm_get_import_list() {
  /*
   * For the eDoc service, we need to import meetings from xml files.
   * We will enumerate the list with the publication-id
   * as parsed from the dir name of the manifest.
   */
  $uris = array();
  foreach (file_scan_directory(variable_get('os2web_edoc_esdh_mm_path', MM_IMPORT_DIR), '/\.xml$/i', array('recurse' => TRUE)) as $xml_file) {
    $uris[] = array('id' => dirname($xml_file->uri), 'uri' => $xml_file->uri);
  }
error_log(print_r($xml_file, 1 ));
  return $uris;
}

/**
 * Imports a meeting, and return the import as a structured array.
 *
 * @param object $meeting
 *   Meeting XML object.
 *
 * @return array
 *   List of meeting_structures to save
 */
function os2web_edoc_esdh_mm_import_meeting($meeting) {
  $file = $meeting['uri'];
  if ($file === FALSE) {
    os2web_meetings_order_addenums();
  }
  $meetings = array();
  if (is_file(drupal_realpath($file))) {
    $manifest = simplexml_load_file(drupal_realpath($file));
    if (is_object($manifest)) {
      $committee = (string) array_shift($manifest->xpath("/Meeting/Committee"));
      $meeting_date = (string) array_shift($manifest->xpath("/Meeting/MeetingDate"));
      $directory_name = dirname(drupal_realpath($file));

      // Check if the committee is allowed to publish.
      $publish_committee = array_map('trim', explode(',', variable_get('os2web_meetings_approved_committee', MM_DEFAULT_APPROVED)));
      if ($committee == '' || !in_array($committee, $publish_committee)) {
        watchdog('eDoc MM', 'Ignored agenda from %committee .', array('%committee' => $committee));
        return FALSE;
      }

      // Fix dates.
      $date_obj = date_create($meeting_date);
      date_timezone_set($date_obj, timezone_open('UTC'));
      $meeting_date = date_format($date_obj, 'Y-m-d H:i');

      // Handle type of meeting.
        // Type:
        // switch ($info['type']) {
        //   case 1:
        //     $meeting['type'] = 'Dagsorden';
        //     break;

        //   case 2:
        //     $meeting['type'] = 'Referat';
        //     break;

        //   default:
        //     $meeting['type'] = 'Anden';
        //     watchdog('eDoc MM', 'Unknown meeting type value: %type', array('%type' => $meeting['type']), WATCHDOG_WARNING);
        // }

      $meeting = array(
        'sys_id' => $directory_name,
        'committee' => $committee,
        'title' => $committee,
        'meeting_date' => $meeting_date,
      );
error_log(print_r(array('committee' => $committee, 'date' => $meeting_date), 1));
      $meeting_agendas = $manifest->xpath("/Meeting/MeetingAgendaTypes");
      if (empty($meeting_agendas)) {
        watchdog('eDoc MM', 'Empty list of import items in %file.', array('%file' => $file), WATCHDOG_WARNING);
      }

      foreach ($meeting_agendas as $meeting_agenda) {

        $imported = _os2web_edoc_esdh_mm_import_meeting_agenda($meeting, $meeting_agenda);

        if (is_array($imported)) {
          $imported['publish'] = TRUE;
          $meetings[] = $imported;
        }
        // else {
        //   // Publication not published or public.
        //   $meetings[] = array(
        //     'system_id' => $agendainfo['sysid'],
        //     'publish' => FALSE,
        //   ) + os2web_esdh_provider_default_meeting();
        // }
      }
    }
    else {
      watchdog('eDoc MM', 'Failed to parse XML in %file during import.', array('%file' => $file), WATCHDOG_WARNING);
    }
  }
  return $meetings;
}

/**
 * Imports a meeting from filesystem.
 *
 * @param array $meeting
 *   Meeting array.
 *
 * @param SimpleXMLElement $agenda_xml
 *   Meeting agenda XML object.
 *
 * @return mixed
 *   Meeting_struct array for an accepted import, else FALSE
 */
function _os2web_edoc_esdh_mm_import_meeting_agenda(&$meeting, $agenda_xml) {
  $xml_item_pdf = (string) array_shift($agenda_xml->xpath('/PDFDocument'));
  $xml_item_type = (string) array_shift($agenda_xml->xpath('/AgendaType'));

  $meeting['items'] = array();
  $meeting['type'] = $xml_item_type;


  $pdf_uri = implode('/', array(
    variable_get('os2web_edoc_esdh_mm_path', MM_IMPORT_DIR),
    $meeting['sys_id'],
    $xml_item_pdf,
  ));

error_log(print_r($agendainfo, 1));

  // Only add the agenda if its PDF exists.
  if (file_exists(drupal_realpath($pdf_uri))) {
    $agenda['full_doc'] = array(
      'uri' => $pdf_uri,
      'title' => $meeting['committee'],
    );
    // Import all agenda items.
    foreach ($agenda_xml->xpath("/MeetingAgendaItems") as $agenda_item) {
      if (($item = _os2web_edoc_esdh_mm_import_meeting_agenda_item($meeting, $agenda_item)) !== FALSE) {
        $agenda['items'][] = $item;
      }
    }
    // Apply defaults, just in case we missed something.
    $agenda += os2web_esdh_provider_default_meeting();
    return $agenda;
  }
  watchdog('eDoc MM', 'File do not exist in folder. %file', array('%file' => $xml_item_pdf), WATCHDOG_WARNING);
  return FALSE;
}

/**
 * Imports a single item for an agenda.
 *
 * @param array $meeting
 *   Meeting array.
 *
 * @param SimpleXMLElement $xml
 *   Xml segment containing the item
 *
 * @return array
 *   The imported item.
 */
function _os2web_edoc_esdh_mm_import_meeting_agenda_item($meeting, $xml) {
  $xml_item_number = (int) array_shift($xml->xpath("/AgendaItemNumber"));
  $xml_item_pdf = (string) array_shift($agenda_xml->xpath('/PDFDocument'));
  $xml_item_name = (string) array_shift($agenda_xml->xpath('/Document/NeutralTitle'));
  $document_type = (string) array_shift($agenda_xml->xpath('/Document/PublishingType'));

  $item = array(
    'id' => $xml_item_number,
    'title' => 'Punkt ' . $xml_item_number . ' ' . $xml_item_name,
    'access' => $document_type,
    'bullets' => array(),
    'enclosures' => array(),
    'doc' => array(
      'uri' => implode('/', array(
        variable_get('os2web_edoc_esdh_mm_path', MM_IMPORT_DIR),
        $meeting['sys_id'],
        $xml_item_pdf)),
      'title' => $xml_item_name,
    ),
  );
  // Deny closed content.
  if ($document_type === 'SKAL PUBLICERES') {
    // Process attachments.
    // foreach ($xml->xpath("table[@name='bullet']") as $bullet_xml) {
    //   if (($bullet = _os2web_edoc_esdh_import_bullet($info, $bullet_xml)) !== FALSE) {
    //     if (is_int($bullet['sort']) && $bullet['sort'] > 0) {
    //       $item['bullets'][$bullet['sort'] - 1] = $bullet;
    //     }
    //     else {
    //       $item['bullets'][] = $bullet;
    //     }
    //   }
    // }
    // Process attachments.
    foreach ($xml->xpath("/Document/Attachments']") as $enclosure) {
      $filename_in = (string) array_shift($enclosure->xpath("/PDFDocument"));
      $access = (string) array_shift($enclosure->xpath("/PublishingType"));
      if (strlen($filename_in) > 0 && $access === 'SKAL PUBLICERES') {
        $item['enclosures'][] = array(
          'uri' => implode('/', array(
            variable_get('os2web_edoc_esdh_mm_path', MM_IMPORT_DIR),
            $info['filesfolder'],
            $filename_in)),
          'title' => trim((string) array_shift($enclosure->xpath("/NeutralTitle"))),
          'access' => TRUE,
        );
      }
    }
  }
  $item += os2web_esdh_provider_default_item();
  return $item;
}

/**
 * Case insensitive fileexists(). Code from comments on php.net.
 *
 * @param string $filename
 *   File name to check.
 *
 * @return mixed
 *   False if file not found, case corrected filename if found.
 */
function _os2web_edoc_esdh_similar_file_exists($filename) {
  if (file_exists($filename)) {
    return $filename;
  }
  $lcase_filename = strtolower($filename);
  foreach (glob(dirname($filename) . '/*') as $file) {
    if (strtolower($file) == $lcase_filename) {
      return $file;
    }
  }
  return FALSE;
}

/**
 * This function will set up the addenum connections for meetings.
 * @TODO: Rid this of drupal specific code.
 */
function _os2web_edoc_esdh_mm_order_addenums($drush = FALSE) {
  if (!lock_acquire(__FUNCTION__)) {
    return;
  }

  // Debug bit set in os2web_esdh_provider settings.
  $debug = variable_get('os2web_esdh_provider_debug_info');
  $debug && timer_start('_os2web_edoc_esdh_mm_order_addenums') && error_log('MM Import - Start of addendums - ' . timer_read('_os2web_edoc_esdh_mm_order_addenums') . 'ms - ' . memory_get_usage());

  $rev_meetings = array();

  // Find all meetings and gather meeting and agenda_ids.
  $query = new EntityFieldQuery();
  $query->entityCondition('entity_type', 'node', '=')
        ->propertyCondition('type', 'meeting', '=');
  $nodes_result = $query->execute();

  if (!isset($nodes_result['node'])) {
    return;
  }
  $nodes = $nodes_result['node'];

  // Load necessary fields into the node object.
  $fields = field_info_instances('node', 'meeting');
  field_attach_load('node', $nodes, FIELD_LOAD_CURRENT, array('field_id' => $fields['field_addenum_to']['field_id']));
  field_attach_load('node', $nodes, FIELD_LOAD_CURRENT, array('field_id' => $fields['field_meeting_id']['field_id']));
  field_attach_load('node', $nodes, FIELD_LOAD_CURRENT, array('field_id' => $fields['field_meeting_sub_id']['field_id']));

  // Meetings as: $meetings[<meeting_id>][<system_id>][<node_id>] .
  $meetings = array();

  $nids_with_addendums = array();

  $debug && error_log('MM Import - Retrieve nodes with addendums from ' . count($nodes) . ' nodes - ' . timer_read('_os2web_edoc_esdh_mm_order_addenums') . 'ms - ' . memory_get_usage());
  // Loop through all meeting nodes, and generate an array with meeting grouped.
  foreach ($nodes as $node) {
    $addenum_to = field_get_items('node', $node, 'field_addenum_to');
    if (isset($addenum_to[0]['nid']) && is_numeric($addenum_to[0]['nid'])) {
      $nids_with_addendums[$node->nid] = $node->nid;
    }
    $meeting_id = field_get_items('node', $node, 'field_meeting_id');
    $meeting_sub_id = field_get_items('node', $node, 'field_meeting_sub_id');
    if (!isset($meeting_id[0]['value']) || !isset($meeting_sub_id[0]['value'])) {
      $debug && error_log('MM Import - Missing meeting_id or meeting_sub_id for nid=' . $node->nid);
      continue;
    }
    if (isset($meetings[$meeting_id[0]['value']][$meeting_sub_id[0]['value']])) {
      // Duplicate meeting detected.
      watchdog('eDoc MM', 'Duplicate import found for meeting %mid. Deleting nid=%nid.', array('%mid' => $meeting_id['value'], '%nid' => $node->nid), WATCHDOG_WARNING);
      node_delete($node->nid);
    }
    else {
      $meetings[$meeting_id[0]['value']][$meeting_sub_id[0]['value']] = $node->nid;
    }
  }

  $debug && error_log('MM Import - Update addendums on ' . count($meetings) . ' nodes - ' . timer_read('_os2web_edoc_esdh_mm_order_addenums') . 'ms - ' . memory_get_usage());
  // Run through the meeting array, to determine the addendums.
  foreach ($meetings as $m_id => $meeting) {
    // Lowest system id first.
    ksort($meeting);
    if (in_array($m_id, $rev_meetings)) {
      $meeting = array_reverse($meeting);
    }
    // Lowest system id is primary.
    $primary = $nodes[array_shift($meeting)];
    $addenum_to = field_get_items('node', $primary, 'field_addenum_to');
    if (!empty($addenum_to[0]['nid'])) {
      $primary->field_addenum_to[LANGUAGE_NONE] = array();
      node_save($primary);
    }
    while ($next = array_shift($meeting)) {
      // Set up references for addenum agendas.
      $node = $nodes[$next];
      if ($primary) {
        $addenum_to = field_get_items('node', $node, 'field_addenum_to');
        $addenum_to = is_array($addenum_to) ? array_shift(array_shift($addenum_to)) : FALSE;
        // Save time by only updating what is needed.
        if ($addenum_to != $primary->nid) {
          // error_log(basename(__FILE__) . ':' . __LINE__ . ' Var: $primary = ' . print_r($primary->nid, 1));
          // error_log(basename(__FILE__) . ':' . __LINE__ . ' Var: $addenum_to = ' . print_r($addenum_to, 1));
          $node->field_addenum_to[LANGUAGE_NONE][0]['nid'] = $primary->nid;
          node_save($node);
        }
      }
      else {
        error_log('MARK - ' . basename(__FILE__) . ':' . __LINE__ . ' in ' . __FUNCTION__ . '()');
        error_log(basename(__FILE__) . ':' . __LINE__ . " jm@bellcom.dk: wanted to delete $node->nid");
        // node_delete($node->nid);
      }
      // Remove node from final nids_with_addendums.
      unset($nids_with_addendums[$node->nid]);
      if ($drush === TRUE) {
        if ($primary) {
          drush_log("Updated meeting $m_id..", 'ok');
        }
        else {
          drush_log("Removed stale addenum $m_id..", 'ok');
        }
      }
    }
  }

  $debug && error_log('MM Import - Start reset addendums on ' . count($nids_with_addendums) . ' nodes - ' . timer_read('_os2web_edoc_esdh_mm_order_addenums') . 'ms - ' . memory_get_usage());
  // Remove any addendums from nodes which hasnt been proccessed.
  foreach ($nids_with_addendums as $nid) {
    // Load the node from the local variable.
    $node = $nodes[$nid];
    $node->field_addenum_to = array();
    node_save($node);
    if ($drush === TRUE) {
      drush_log("Resetting $nid..", 'ok');
    }
  }
  $debug && error_log('MM Import - End of addendums - ' . timer_read('_os2web_edoc_esdh_mm_order_addenums') . 'ms - ' . memory_get_usage());
  lock_release(__FUNCTION__);
}
