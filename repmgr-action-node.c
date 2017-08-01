/*
 * repmgr-action-node.c
 *
 * Implements actions available for any kind of node
 *
 * Copyright (c) 2ndQuadrant, 2010-2017
 */

#include <sys/stat.h>
#include <dirent.h>

#include "repmgr.h"

#include "repmgr-client-global.h"
#include "repmgr-action-node.h"

static bool copy_file(const char *src_file, const char *dest_file);
static void format_archive_dir(char *archive_dir);

void
do_node_status(void)
{
	PGconn	   	   *conn;

	int 			target_node_id = UNKNOWN_NODE_ID;
	t_node_info 	node_info = T_NODE_INFO_INITIALIZER;
	char			server_version[MAXLEN];
	char			cluster_size[MAXLEN];
	PQExpBufferData	output;

	KeyValueList	node_status = { NULL, NULL };
	KeyValueListCell *cell;

	ItemList 		warnings = { NULL, NULL };
	RecoveryType	recovery_type;
	ReplInfo 		replication_info = T_REPLINFO_INTIALIZER;

	if (strlen(config_file_options.conninfo))
		conn = establish_db_connection(config_file_options.conninfo, true);
	else
		conn = establish_db_connection_by_params(&source_conninfo, true);

	server_version_num = get_server_version(conn, NULL);

	if (runtime_options.node_id != UNKNOWN_NODE_ID)
		target_node_id = runtime_options.node_id;
	else
		target_node_id = config_file_options.node_id;

	/* Check node exists and is really a standby */

	if (get_node_record(conn, target_node_id, &node_info) != RECORD_FOUND)
	{
		log_error(_("no record found for node %i"), target_node_id);
		PQfinish(conn);
		exit(ERR_BAD_CONFIG);
	}
	(void) get_server_version(conn, server_version);

	if (get_cluster_size(conn, cluster_size) == false)
		strncpy(cluster_size, _("unknown"), MAXLEN);

	recovery_type = get_recovery_type(conn);

	get_node_replication_stats(conn, &node_info);

	key_value_list_set(
		&node_status,
		"PostgreSQL version",
		server_version);

	key_value_list_set(
		&node_status,
		"Total data size",
		cluster_size);

	key_value_list_set(
		&node_status,
		"Conninfo",
		node_info.conninfo);

	key_value_list_set(
		&node_status,
		"Role",
		get_node_type_string(node_info.type));

	switch (node_info.type)
	{
		case PRIMARY:
			if (recovery_type == RECTYPE_STANDBY)
			{
				item_list_append(
					&warnings,
					_("- node is registered as primary but running as standby"));
			}
			break;
		case STANDBY:
			if (recovery_type == RECTYPE_PRIMARY)
			{
				item_list_append(
					&warnings,
					_("- node is registered as standby but running as primary"));
			}
			break;
		case BDR:
		default:
			break;
	}


	if (node_info.max_wal_senders >= 0)
	{
		key_value_list_set_format(
			&node_status,
			"Replication connections",
			"%i (of maximal %i)",
			node_info.attached_wal_receivers,
			node_info.max_wal_senders);
	}
	else if (node_info.max_wal_senders == 0)
	{
		key_value_list_set_format(
			&node_status,
			"Replication connections",
			"disabled");
	}

	if (node_info.max_replication_slots > 0)
	{
		PQExpBufferData	slotinfo;
		initPQExpBuffer(&slotinfo);

		appendPQExpBuffer(
			&slotinfo,
			"%i (of maximal %i)",
			node_info.active_replication_slots + node_info.inactive_replication_slots,
			node_info.max_replication_slots);


		if (node_info.inactive_replication_slots > 0)
		{
			appendPQExpBuffer(
				&slotinfo,
				"; %i inactive",
				node_info.inactive_replication_slots);

			item_list_append_format(
				&warnings,
				_("- node has %i inactive replication slots"),
				node_info.inactive_replication_slots);
		}

		key_value_list_set(
			&node_status,
			"Replication slots",
			slotinfo.data);

		termPQExpBuffer(&slotinfo);
	}
	else if (node_info.max_replication_slots == 0)
	{
		key_value_list_set(
			&node_status,
			"Replication slots",
			"disabled");
	}


	// if standby (and in recovery), show:
	// upstream
	// -> check if matches expected; parse recovery.conf for < 9.6 (must be superuser),
	//   otherwise use pg_stat_wal_receiver
	// streaming/in archive recovery/disconnected
	// last received
	// last replayed
	// lag if streaming, or if in recovery can compare with upstream

	if (node_info.type == STANDBY)
	{
		get_replication_info(conn, &replication_info);

		key_value_list_set_format(
			&node_status,
			"Last received LSN",
			"%X/%X", format_lsn(replication_info.last_wal_receive_lsn));

		key_value_list_set_format(
			&node_status,
			"Last replayed LSN",
			"%X/%X", format_lsn(replication_info.last_wal_replay_lsn));
	}
	else
	{
		key_value_list_set(
			&node_status,
			"Last received LSN",
			"");
		key_value_list_set(
			&node_status,
			"Last replayed LSN",
			"");

	}

	initPQExpBuffer(&output);

	if (runtime_options.csv == true)
	{
		/* output header */
		appendPQExpBuffer(
			&output,
			"\"Node name\",\"Node ID\",");

		for (cell = node_status.head; cell; cell = cell->next)
		{
			appendPQExpBuffer(
				&output,
				"\"%s\",", cell->key);
		}

		/* we'll add the raw data as well */
		appendPQExpBuffer(
			&output,
			"\"max_walsenders\",\"occupied_walsenders\",\"max_replication_slots\",\"active_replication_slots\",\"inactive_replaction_slots\"\n");

		/* output data */
		appendPQExpBuffer(
			&output,
			"\"%s\",%i,",
			node_info.node_name,
			node_info.node_id);

		for (cell = node_status.head; cell; cell = cell->next)
		{
			appendPQExpBuffer(
				&output,
				"\"%s\",", cell->value);
		}

		appendPQExpBuffer(
			&output,
			"%i,%i,%i,%i,%i\n",
			node_info.max_wal_senders,
			node_info.attached_wal_receivers,
			node_info.max_replication_slots,
			node_info.active_replication_slots,
			node_info.inactive_replication_slots);
	}
	else
	{
		appendPQExpBuffer(
			&output,
			"Node \"%s\":\n",
			node_info.node_name);

		for (cell = node_status.head; cell; cell = cell->next)
		{
			appendPQExpBuffer(
				&output,
				"\t%s: %s\n", cell->key, cell->value);
		}
	}

	puts(output.data);

	termPQExpBuffer(&output);

	if (warnings.head != NULL && runtime_options.terse == false)
	{
		log_warning(_("following issue(s) were detected:"));
		print_item_list(&warnings);
		/* add this when functionality implemented */
		/* log_hint(_("execute \"repmgr node check\" for more details")); */
	}

}


void
do_node_check(void)
{
}


/*
 * Intended mainly for "internal" use by `node switchover`, which
 * calls this on the target server to archive any configuration files
 * in the data directory, which may be overwritten by an operation
 * like pg_rewind
 *
 * Requires configuration file, optionally --config_archive_dir
 */
void
do_node_archive_config(void)
{
	char archive_dir[MAXPGPATH];
	struct stat statbuf;
	struct dirent *arcdir_ent;
	DIR			  *arcdir;

	PGconn	   *local_conn = NULL;
	KeyValueList	config_files = { NULL, NULL };
	KeyValueListCell *cell;
	int  copied_count = 0;

	format_archive_dir(archive_dir);

	/* sanity-check directory path */
	if (stat(archive_dir, &statbuf) == -1)
	{
		if (errno != ENOENT)
		{
			log_error(_("error encountered when checking archive directory \"%s\""),
					  archive_dir);
			log_detail("%s", strerror(errno));
			exit(ERR_BAD_CONFIG);
		}

		/* attempt to create and open the directory */
		if (mkdir(archive_dir, S_IRWXU) != 0 && errno != EEXIST)
		{
			log_error(_("unable to create temporary archive directory \"%s\""),
					  archive_dir);
			log_detail("%s", strerror(errno));
			exit(ERR_BAD_CONFIG);
		}


	}
	else if(!S_ISDIR(statbuf.st_mode))
    {
		log_error(_("\"%s\" exists but is not a directory"),
				  archive_dir);
		exit(ERR_BAD_CONFIG);
	}


	arcdir = opendir(archive_dir);

	if (arcdir == NULL)
	{
		log_error(_("unable to open archive directory \"%s\""),
				  archive_dir);
		log_detail("%s", strerror(errno));
		exit(ERR_BAD_CONFIG);
	}

	/*
	 * attempt to remove any existing files in the directory
	 * TODO: collate problem files into list
	 */
	while ((arcdir_ent = readdir(arcdir)) != NULL)
	{
		char arcdir_ent_path[MAXPGPATH];

		snprintf(arcdir_ent_path, MAXPGPATH,
				 "%s/%s",
				 archive_dir,
				 arcdir_ent->d_name);


		if (stat(arcdir_ent_path, &statbuf) == 0 && !S_ISREG(statbuf.st_mode))
		{
			continue;
		}


		if (unlink(arcdir_ent_path) == -1)
		{
			log_error(_("unable to create temporary archive directory \"%s\""),
					  archive_dir);
			log_detail("%s", strerror(errno));
			closedir(arcdir);
			exit(ERR_BAD_CONFIG);
		}
	}

	closedir(arcdir);

	local_conn = establish_db_connection(config_file_options.conninfo, true);

	get_datadir_configuration_files(local_conn, &config_files);

	for (cell = config_files.head; cell; cell = cell->next)
	{
		char dest_file[MAXPGPATH] = "";

		snprintf(dest_file, MAXPGPATH,
				 "%s/%s",
				 archive_dir,
				 cell->key);

		copy_file(cell->value, dest_file);
		copied_count++;
	}

	PQfinish(local_conn);

	log_verbose(LOG_INFO, _("%i files copied to %s"),
				copied_count, archive_dir);
}


/*
 * Intended mainly for "internal" use by `standby switchover`, which
 * calls this on the target server to restore any configuration files
 * to the data directory, which may have been overwritten by an operation
 * like pg_rewind
 *
 * Not designed to be called if the instance is running, but does
 * not currently check.
 *
 * Requires -D/--pgdata, optionally --config_archive_dir
 *
 * Removes --config_archive_dir after successful copy
 */

void
do_node_restore_config(void)
{
	char archive_dir[MAXPGPATH];

	DIR			  *arcdir;
	struct dirent *arcdir_ent;
	int			   copied_count = 0;
	bool		   copy_ok = true;

	format_archive_dir(archive_dir);

	arcdir = opendir(archive_dir);

	if (arcdir == NULL)
	{
		log_error(_("unable to open archive directory \"%s\""),
				  archive_dir);
		log_detail("%s", strerror(errno));
		exit(ERR_BAD_CONFIG);
	}

	while ((arcdir_ent = readdir(arcdir)) != NULL) {
		struct stat statbuf;
		char src_file_path[MAXPGPATH];
		char dest_file_path[MAXPGPATH];

		snprintf(src_file_path, MAXPGPATH,
				 "%s/%s",
				 archive_dir,
				 arcdir_ent->d_name);

		/* skip non-files */
		if (stat(src_file_path, &statbuf) == 0 && !S_ISREG(statbuf.st_mode))
		{
			continue;
		}

		snprintf(dest_file_path, MAXPGPATH,
				 "%s/%s",
				 runtime_options.data_dir,
				 arcdir_ent->d_name);

		log_verbose(LOG_DEBUG, "copying \"%s\" to \"%s\"", src_file_path, dest_file_path);

		if (copy_file(src_file_path, dest_file_path) == false)
		{
			copy_ok = false;
			log_warning(_("unable to copy \"%s\" to \"%s\""),
						arcdir_ent->d_name, runtime_options.data_dir);
		}
		else
		{
			unlink(src_file_path);
			copied_count++;
		}

	}
	closedir(arcdir);


	if (copy_ok == false)
	{
		log_error(_("unable to copy all files from %s"), archive_dir);
		exit(ERR_BAD_CONFIG);
	}

	log_notice(_("%i files copied to %s"), copied_count, runtime_options.data_dir);

	/*
	 * Finally, delete directory - it should be empty unless it's been interfered
	 * with for some reason, in which case manual intervention is required
	 */
	if (rmdir(archive_dir) != 0 && errno != EEXIST)
	{
		log_warning(_("unable to delete %s"), archive_dir);
		log_detail(_("directory may need to be manually removed"));
	}
	else
	{
		log_verbose(LOG_NOTICE, "directory %s deleted", archive_dir);
	}

	return;
}


static void
format_archive_dir(char *archive_dir)
{
	snprintf(archive_dir,
			 MAXPGPATH,
			 "%s/repmgr-config-archive-%s",
			 runtime_options.config_archive_dir,
			 config_file_options.node_name);

	log_verbose(LOG_DEBUG, "using archive directory \"%s\"", archive_dir);
}


static bool
copy_file(const char *src_file, const char *dest_file)
{
	FILE  *ptr_old, *ptr_new;
	int  a;

	ptr_old = fopen(src_file, "r");
	ptr_new = fopen(dest_file, "w");

	if (ptr_old == NULL)
		return false;

	if (ptr_new == NULL)
	{
		fclose(ptr_old);
		return false;
	}

	chmod(dest_file, S_IRUSR | S_IWUSR);

	while(1)
	{
		a = fgetc(ptr_old);

		if (!feof(ptr_old))
		{
			fputc(a, ptr_new);
		}
		else
		{
			break;
		}
	}

	fclose(ptr_new);
	fclose(ptr_old);

	return true;
}
