#include "include.h"

// FIX THIS: test whether this works with burp2
int restore_interrupt(struct asfd *asfd,
	struct sbuf *sb, const char *msg, struct conf *conf)
{
	int ret=0;
	struct cntr *cntr=conf->cntr;
	struct iobuf *rbuf=asfd->rbuf;

	if(!cntr) return 0;

	cntr_add(cntr, CMD_WARNING, 1);
	logp("WARNING: %s\n", msg);
	if(asfd->write_str(asfd, CMD_WARNING, msg)) goto end;

	// If it is file data, get the server
	// to interrupt the flow and move on.
	if(sb->path.cmd!=CMD_FILE
	  && sb->path.cmd!=CMD_ENC_FILE
	  && sb->path.cmd!=CMD_EFS_FILE
	  && sb->path.cmd!=CMD_VSS
	  && sb->path.cmd!=CMD_ENC_VSS
	  && sb->path.cmd!=CMD_VSS_T
	  && sb->path.cmd!=CMD_ENC_VSS_T)
		return 0;
	if(sb->burp1 && !(sb->burp1->datapth.buf))
		return 0;
	if(sb->burp2 && !(sb->path.buf))
		return 0;

	if(asfd->write_str(asfd, CMD_INTERRUPT, sb->burp1->datapth.buf))
		goto end;

	// Read to the end file marker.
	while(1)
	{
		iobuf_free_content(rbuf);
		if(asfd->read(asfd))
			goto end;
		if(!ret && rbuf->len)
		{
			if(rbuf->cmd==CMD_APPEND)
				continue;
			else if(rbuf->cmd==CMD_END_FILE)
				break;
			else
			{
				iobuf_log_unexpected(rbuf, __func__);
				goto end;
			}
		}
	}

	ret=0;
end:
	iobuf_free_content(rbuf);
	return ret;
}

static int open_for_restore(struct asfd *asfd,
	BFILE *bfd,
	const char *path,
	struct sbuf *sb,
	int vss_restore,
	struct conf *conf)
{
	static int flags;
	bclose(bfd, asfd);
	binit(bfd, sb->winattr, conf);
#ifdef HAVE_WIN32
	if(vss_restore)
		set_win32_backup(bfd);
	else
		bfd->use_backup_api=0;
#endif
	if(S_ISDIR(sb->statp.st_mode))
	{
		// Windows directories are treated as having file data.
		flags=O_WRONLY|O_BINARY;
		mkdir(path, 0777);
	}
	else
		flags=O_WRONLY|O_BINARY|O_CREAT|O_TRUNC;

	if(bopen(bfd, asfd, path, flags, S_IRUSR | S_IWUSR)<=0)
	{
		berrno be;
		char msg[256]="";
		snprintf(msg, sizeof(msg), "Could not open for writing %s: %s",
			path, be.bstrerror(errno));
		if(restore_interrupt(asfd, sb, msg, conf))
			return -1;
	}
	// Add attributes to bfd so that they can be set when it is closed.
	bfd->winattr=sb->winattr;
	memcpy(&bfd->statp, &sb->statp, sizeof(struct stat));
	return 0;
}

static int start_restore_file(struct asfd *asfd,
	BFILE *bfd,
	struct sbuf *sb,
	const char *fname,
	enum action act,
	const char *encpassword,
	char **metadata,
	size_t *metalen,
	int vss_restore,
	struct conf *conf)
{
	int ret=-1;
	char *rpath=NULL;

	if(act==ACTION_VERIFY)
	{
		cntr_add(conf->cntr, sb->path.cmd, 1);
		return 0;
	}

	if(build_path(fname, "", &rpath, NULL))
	{
		char msg[256]="";
		// failed - do a warning
		snprintf(msg, sizeof(msg), "build path failed: %s", fname);
		if(restore_interrupt(asfd, sb, msg, conf))
			ret=-1;
		ret=0; // Try to carry on with other files.
		goto end;
	}

	if(open_for_restore(asfd, bfd, rpath, sb, vss_restore, conf))
		goto end;

	cntr_add(conf->cntr, sb->path.cmd, 1);

	ret=0;
end:
	if(rpath) free(rpath);
	return ret;
}

int restore_dir(struct asfd *asfd,
	struct sbuf *sb, const char *dname, enum action act, struct conf *conf)
{
	int ret=0;
	char *rpath=NULL;
	if(act==ACTION_RESTORE)
	{
		if(build_path(dname, "", &rpath, NULL))
		{
			char msg[256]="";
			// failed - do a warning
			snprintf(msg, sizeof(msg),
				"build path failed: %s", dname);
			if(restore_interrupt(asfd, sb, msg, conf))
				ret=-1;
			goto end;
		}
		else if(!is_dir_lstat(rpath))
		{
			if(mkdir(rpath, 0777))
			{
				char msg[256]="";
				snprintf(msg, sizeof(msg), "mkdir error: %s",
					strerror(errno));
				// failed - do a warning
				if(restore_interrupt(asfd, sb, msg, conf))
					ret=-1;
				goto end;
			}
		}
		attribs_set(asfd, rpath, &(sb->statp), sb->winattr, conf);
		if(!ret) cntr_add(conf->cntr, sb->path.cmd, 1);
	}
	else cntr_add(conf->cntr, sb->path.cmd, 1);
end:
	if(rpath) free(rpath);
	return ret;
}

/*
static int restore_metadata(
#ifdef HAVE_WIN32
	BFILE *bfd,
#endif
	struct sbuf *sb,
	const char *fname,
	enum action act,
	const char *encpassword,
	int vss_restore,
	struct conf *conf)
{
	// If it is directory metadata, try to make sure the directory
	// exists. Pass in NULL as the cntr, so no counting is done.
	// The actual directory entry will be coming after the metadata,
	// annoyingly. This is because of the way that the server is queuing
	// up directories to send after file data, so that the stat info on
	// them gets set correctly.
	if(act==ACTION_RESTORE)
	{
		size_t metalen=0;
		char *metadata=NULL;
		if(S_ISDIR(sb->statp.st_mode)
		  && restore_dir(asfd, sb, fname, act, NULL))
			return -1;

		// Read in the metadata...
		if(restore_file_or_get_meta(
#ifdef HAVE_WIN32
			bfd,
#endif
			sb, fname, act, encpassword,
			&metadata, &metalen, vss_restore, conf))
				return -1;
		if(metadata)
		{
			
			if(set_extrameta(
#ifdef HAVE_WIN32
				bfd,
#endif
				fname, sb->path.cmd,
				&(sb->statp), metadata, metalen, conf))
			{
				free(metadata);
				// carry on if we could not do it
				return 0;
			}
			free(metadata);
#ifndef HAVE_WIN32
			// set attributes again, since we just diddled with
			// the file
			attribs_set(fname, &(sb->statp), sb->winattr, conf);
#endif
			cntr_add(conf->cntr, sb->path.cmd, 1);
		}
	}
	else cntr_add(conf->cntr, sb->cmd, 1);
	return 0;
}
*/

int restore_switch_burp2(struct asfd *asfd, struct sbuf *sb,
	const char *fullpath, enum action act,
	BFILE *bfd, int vss_restore, struct conf *conf)
{
	switch(sb->path.cmd)
	{
		case CMD_FILE:
			// Have it a separate statement to the
			// encrypted version so that encrypted and not
			// encrypted files can be restored at the
			// same time.
			if(start_restore_file(asfd,
				bfd, sb, fullpath, act,
				NULL, NULL, NULL,
				vss_restore, conf))
			{
				logp("restore_file error\n");
				goto error;
			}
			break;
/* FIX THIS: Encryption currently not working in burp2
		case CMD_ENC_FILE:
			if(start_restore_file(asfd,
				bfd, sb, fullpath, act,
				conf->encryption_password,
				NULL, NULL, vss_restore, conf))
			{
				logp("restore_file error\n");
				goto error;
			}
			break;
*/
/* FIX THIS: Metadata and EFS not supported yet.
		case CMD_METADATA:
			if(restore_metadata(
				bfd, sb, fullpath, act,
				NULL, vss_restore, conf))
					goto error;
			break;
		case CMD_ENC_METADATA:
			if(restore_metadata(
				bfd, sb, fullpath, act,
				conf->encryption_password,
				vss_restore, conf))
					goto error;
			break;
		case CMD_EFS_FILE:
			if(start_restore_file(asfd,
				bfd, sb,
				fullpath, act,
				NULL,
				NULL, NULL, vss_restore, conf))
			{
				logp("restore_file error\n");
				goto error;
			}
			break;
*/
		default:
			logp("unknown cmd: %c\n", sb->path.cmd);
			goto error;
	}
	return 0;
error:
	return -1;
}