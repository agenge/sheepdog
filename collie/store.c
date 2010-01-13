/*
 * Copyright (C) 2009 Nippon Telegraph and Telephone Corporation.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <mntent.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/xattr.h>
#include <sys/statvfs.h>

#include "collie.h"
#include "meta.h"

#define ANAME_LAST_OID "user.sheepdog.last_oid"
#define ANAME_COPIES "user.sheepdog.copies"
#define ANAME_CURRENT "user.sheepdog.current"

static char *vdi_path;
static char *obj_path;
static char *epoch_path;
static char *mnt_path;

static char *zero_block;

static mode_t def_dmode = S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IWGRP | S_IXGRP;
static mode_t def_fmode = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP;

int nr_sobjs;
struct work_queue *dobj_queue;

static int stat_sheep(uint64_t *store_size, uint64_t *store_free)
{
	struct statvfs vs;
	int ret;
	DIR *dir;
	struct dirent *d;
	uint64_t used = 0;
	struct stat s;
	char path[1024];

	ret = statvfs(mnt_path, &vs);
	if (ret)
		return SD_RES_EIO;

	dir = opendir(obj_path);
	if (!dir)
		return SD_RES_EIO;

	while ((d = readdir(dir))) {
		if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, ".."))
			continue;

		snprintf(path, sizeof(path), "%s%s", obj_path, d->d_name);

		ret = stat(path, &s);
		if (ret)
			continue;

		used += s.st_size;
	}

	*store_size = vs.f_frsize * vs.f_bfree;
	*store_free = vs.f_frsize * vs.f_bfree - used;

	return SD_RES_SUCCESS;
}

static int get_obj_list(struct request *req)
{
	DIR *dir;
	struct dirent *d;
	struct sd_obj_req *hdr = (struct sd_obj_req *)&req->rq;
	struct sd_obj_rsp *rsp = (struct sd_obj_rsp *)&req->rp;
	uint64_t oid;
	uint64_t oid_hash;
	uint64_t start_hash = hdr->oid;
	uint64_t end_hash = hdr->cow_oid;
	char path[1024];
	uint64_t *p = (uint64_t *)req->data;
	int nr = 0;

	snprintf(path, sizeof(path), "%s%08u/", obj_path, hdr->obj_ver);

	dir = opendir(path);
	if (!dir)
		return SD_RES_EIO;

	while ((d = readdir(dir))) {
		if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, ".."))
			continue;

		oid = strtoull(d->d_name, NULL, 16);
		oid_hash = fnv_64a_buf(&oid, sizeof(oid), FNV1A_64_INIT);

		if (oid_hash >= start_hash && oid_hash < end_hash) {
			if ((nr + 1) * sizeof(uint64_t) > hdr->data_length)
				break;

			*(p + nr) = oid;
			nr++;
		}
	}
	rsp->data_length = nr * 8;

	return SD_RES_SUCCESS;
}

static int read_from_one(struct cluster_info *cluster, uint64_t oid,
			 unsigned *rlen, void *buf, uint64_t offset)
{
	int i, n, nr, fd, ret;
	unsigned wlen;
	char name[128];
	struct sheepdog_node_list_entry *e;
	struct sd_obj_req hdr;
	struct sd_obj_rsp *rsp = (struct sd_obj_rsp *)&hdr;

	e = zalloc(SD_MAX_NODES * sizeof(struct sheepdog_node_list_entry));
again:
	nr = build_node_list(&cluster->node_list, e);

	for (i = 0; i < nr; i++) {
		n = obj_to_sheep(e, nr, oid, i);

		snprintf(name, sizeof(name), "%d.%d.%d.%d",
			 e[n].addr[12], e[n].addr[13],
			 e[n].addr[14], e[n].addr[15]);

		fd = connect_to(name, e[n].port);
		if (fd < 0)
			continue;

		memset(&hdr, 0, sizeof(hdr));
		hdr.opcode = SD_OP_READ_OBJ;
		hdr.oid = oid;
		hdr.epoch = cluster->epoch;

		hdr.flags = 0;
		hdr.data_length = *rlen;
		hdr.offset = offset;

		ret = exec_req(fd, (struct sd_req *)&hdr, buf, &wlen, rlen);

		close(fd);

		if (ret)
			continue;

		switch (rsp->result) {
		case SD_RES_SUCCESS:
			return 0;
		case SD_RES_OLD_NODE_VER:
		case SD_RES_NEW_NODE_VER:
			/* waits for the node list timer */
			sleep(2);
			goto again;
			break;
		default:
			;
		}
	}

	free(e);

	return -1;
}

static int read_from_other_sheeps(struct cluster_info *cluster,
				  uint64_t oid, char *buf, int copies)
{
	int ret;
	unsigned int rlen;

	rlen = SD_DATA_OBJ_SIZE;

	ret = read_from_one(cluster, oid, &rlen, buf, 0);

	return ret;
}

static int forward_obj_req(struct cluster_info *cluster, struct request *req)
{
	int i, n, nr, fd, ret;
	unsigned wlen, rlen;
	char name[128];
	struct sd_obj_req *hdr = (struct sd_obj_req *)&req->rq;
	struct sheepdog_node_list_entry *e;
	struct sd_obj_req hdr2;
	struct sd_rsp *rsp = (struct sd_rsp *)&hdr2;
	uint64_t oid = hdr->oid;
	int copies;

	e = zalloc(SD_MAX_NODES * sizeof(struct sheepdog_node_list_entry));
again:
	nr = build_node_list(&cluster->node_list, e);

	copies = hdr->copies;

	/* temporary hack */
	if (!copies)
		copies = cluster->nr_sobjs;

	for (i = 0; i < copies; i++) {
		n = obj_to_sheep(e, nr, oid, i);

		snprintf(name, sizeof(name), "%d.%d.%d.%d",
			 e[n].addr[12], e[n].addr[13],
			 e[n].addr[14], e[n].addr[15]);

		fd = connect_to(name, e[n].port);
		if (fd < 0)
			continue;

		memcpy(&hdr2, hdr, sizeof(hdr2));

		if (hdr->flags & SD_FLAG_CMD_WRITE) {
			wlen = hdr->data_length;
			rlen = 0;
		} else {
			wlen = 0;
			rlen = hdr->data_length;
		}

		hdr2.flags |= SD_FLAG_CMD_FORWARD;

		ret = exec_req(fd, (struct sd_req *)&hdr2, req->data, &wlen, &rlen);

		close(fd);

		if (ret) /* network errors */
			goto again;

		if (hdr->flags & SD_FLAG_CMD_WRITE) {
			if (rsp->result != SD_RES_SUCCESS) {
				free(e);
				return rsp->result;
			}
		} else {
			if (rsp->result == SD_RES_SUCCESS) {
				memcpy(&req->rp, rsp, sizeof(req->rp));
				free(e);
				return SD_RES_SUCCESS;
			}
		}
	}

	free(e);

	return (hdr->flags & SD_FLAG_CMD_WRITE) ? SD_RES_SUCCESS: rsp->result;
}

static int check_epoch(struct cluster_info *cluster, struct request *req)
{
	struct sd_req *hdr = (struct sd_req *)&req->rq;
	uint32_t req_epoch = hdr->epoch;
	uint32_t opcode = hdr->opcode;
	int ret = SD_RES_SUCCESS;

	if (before(req_epoch, cluster->epoch)) {
		ret = SD_RES_OLD_NODE_VER;
		eprintf("old node version %u %u, %x\n",
			cluster->epoch, req_epoch, opcode);
	} else if (after(req_epoch, cluster->epoch)) {
		ret = SD_RES_NEW_NODE_VER;
			eprintf("new node version %u %u %x\n",
				cluster->epoch, req_epoch, opcode);
	}

	return ret;
}

static int ob_open(uint32_t epoch, uint64_t oid, int aflags, int *ret)
{
	char path[1024];
	int flags = O_RDWR | aflags;
	int fd;

	snprintf(path, sizeof(path), "%s%08u/%016" PRIx64, obj_path, epoch, oid);

	fd = open(path, flags, def_fmode);
	if (fd < 0) {
		if (errno == ENOENT)
			*ret = SD_RES_NO_OBJ;
		else
			*ret = SD_RES_UNKNOWN;
	} else
		*ret = 0;

	return fd;
}

static int is_my_obj(struct cluster_info *ci, uint64_t oid, int copies)
{
	int i, n, nr;
	struct sheepdog_node_list_entry e[SD_MAX_NODES];

	nr = build_node_list(&ci->node_list, e);

	for (i = 0; i < copies; i++) {
		n = obj_to_sheep(e, nr, oid, i);
		if (e[n].id == ci->this_node.id)
			return 1;
	}

	return 0;
}

int update_epoch_store(struct cluster_info *ci, uint32_t epoch)
{
	int ret;
	char new[1024], old[1024];
	struct stat s;
	DIR *dir;
	struct dirent *d;
	uint64_t oid;

	snprintf(new, sizeof(new), "%s%08u/", obj_path, epoch);
	mkdir(new, def_dmode);

	snprintf(old, sizeof(old), "%s%08u/", obj_path, epoch - 1);

	ret = stat(old, &s);
	if (ret)
		return 0;

	dir = opendir(old);
	if (!dir) {
		eprintf("%s, %s, %m\n", old, new);
		return 1;
	}

	while ((d = readdir(dir))) {
		if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, ".."))
			continue;

		oid = strtoull(d->d_name, NULL, 16);
		/* TODO: use proper object coipes */
		if (is_my_obj(ci, oid, ci->nr_sobjs)) {
			snprintf(new, sizeof(new), "%s%08u/%s", obj_path, epoch,
				d->d_name);
			snprintf(old, sizeof(old), "%s%08u/%s", obj_path, epoch - 1,
				d->d_name);
			link(old, new);
		}
	}

	closedir(dir);

	return 0;
}

void store_queue_request(struct work *work, int idx)
{
	struct request *req = container_of(work, struct request, work);
	struct cluster_info *cluster = req->ci->cluster;
	char path[1024];
	int fd = -1, ret = SD_RES_SUCCESS;
	char *buf = zero_block + idx * SD_DATA_OBJ_SIZE;
	struct sd_obj_req *hdr = (struct sd_obj_req *)&req->rq;
	struct sd_obj_rsp *rsp = (struct sd_obj_rsp *)&req->rp;
	uint64_t oid = hdr->oid;
	uint32_t opcode = hdr->opcode;
	uint32_t epoch = cluster->epoch;
	uint32_t req_epoch = hdr->epoch;
	struct sd_node_rsp *nrsp = (struct sd_node_rsp *)&req->rp;
	int copies;

	dprintf("%d, %x, %" PRIx64" , %u, %u\n", idx, opcode, oid, epoch, req_epoch);

	if (list_empty(&cluster->node_list)) {
		/* we haven't got SD_OP_GET_NODE_LIST response yet. */
		ret = SD_RES_SYSTEM_ERROR;
		goto out;
	}

	if (opcode != SD_OP_GET_NODE_LIST) {
		ret = check_epoch(cluster, req);
		if (ret != SD_RES_SUCCESS)
			goto out;
	}

	if (opcode == SD_OP_STAT_SHEEP) {
		ret = stat_sheep(&nrsp->store_size, &nrsp->store_free);
		goto out;
	}

	if (opcode == SD_OP_GET_OBJ_LIST) {
		ret = get_obj_list(req);
		goto out;
	}

	if (!(hdr->flags & SD_FLAG_CMD_FORWARD)) {
		ret = forward_obj_req(cluster, req);
		goto out;
	}

	switch (opcode) {
	case SD_OP_CREATE_AND_WRITE_OBJ:
	case SD_OP_WRITE_OBJ:
	case SD_OP_READ_OBJ:
	case SD_OP_SYNC_OBJ:
		if (opcode == SD_OP_CREATE_AND_WRITE_OBJ)
			fd = ob_open(req_epoch, oid, O_CREAT, &ret);
		else
			fd = ob_open(req_epoch, oid, 0, &ret);

		if (fd < 0)
			goto out;

		if (opcode != SD_OP_CREATE_AND_WRITE_OBJ)
			break;

		if (!hdr->copies) {
			eprintf("zero copies is invalid\n");
			ret = SD_RES_INVALID_PARMS;
			goto out;
		}

		ret = ftruncate(fd, 0);
		if (ret) {
			ret = SD_RES_EIO;
			goto out;
		}

		ret = fsetxattr(fd, ANAME_COPIES, &hdr->copies,
				sizeof(hdr->copies), 0);
		if (ret) {
			eprintf("use 'user_xattr' option?\n");
			ret = SD_RES_SYSTEM_ERROR;
			goto out;
		}

		if (!is_data_obj(oid))
			break;

		if (hdr->flags & SD_FLAG_CMD_COW) {
			dprintf("%" PRIu64 "\n", hdr->cow_oid);

			ret = read_from_other_sheeps(cluster,
						     hdr->cow_oid, buf,
						     hdr->copies);
			if (ret) {
				ret = 1;
				goto out;
			}
		} else {
			dprintf("%" PRIu64 "\n", oid);
			memset(buf, 0, SD_DATA_OBJ_SIZE);
		}

		dprintf("%" PRIu64 "\n", oid);

		ret = pwrite64(fd, buf, SD_DATA_OBJ_SIZE, 0);
		if (ret != SD_DATA_OBJ_SIZE) {
			ret = SD_RES_EIO;
			goto out;
		}
	default:
		break;
	}

	switch (opcode) {
	case SD_OP_REMOVE_OBJ:
		snprintf(path, sizeof(path), "%s%" PRIx64, obj_path, oid);
		ret = unlink(path);
		if (ret)
			ret = 1;
		break;
	case SD_OP_READ_OBJ:
		/*
		 * TODO: should be optional (we can use the flags) for
		 * performance; qemu doesn't always need the copies.
		 */
		copies = 0;
		ret = fgetxattr(fd, ANAME_COPIES, &copies, sizeof(copies));
		if (ret != sizeof(copies)) {
			ret = SD_RES_SYSTEM_ERROR;
			goto out;
		}

		ret = pread64(fd, req->data, hdr->data_length, hdr->offset);
		if (ret < 0)
			ret = SD_RES_EIO;
		else {
			rsp->data_length = ret;
			rsp->copies = copies;
			ret = SD_RES_SUCCESS;
		}
		break;
	case SD_OP_CREATE_AND_WRITE_OBJ:
	case SD_OP_WRITE_OBJ:
		ret = pwrite64(fd, req->data, hdr->data_length, hdr->offset);
		if (ret != hdr->data_length) {
			ret = SD_RES_EIO;
			goto out;
		} else
			ret = SD_RES_SUCCESS;
		break;
	case SD_OP_SYNC_OBJ:
		ret = fsync(fd);
		if (ret) {
			if (errno == EIO)
				ret = SD_RES_EIO;
			else
				ret = SD_RES_UNKNOWN;
		}
		break;
	}
out:
	if (ret != SD_RES_SUCCESS) {
		dprintf("failed, %d, %x, %" PRIx64" , %u, %u\n",
			idx, opcode, oid, epoch, req_epoch);
		rsp->result = ret;
	}

	if (fd != -1)
		close(fd);
}

static int so_read_vdis(struct request *req)
{
	struct sd_so_rsp *rsp = (struct sd_so_rsp *)&req->rp;
	DIR *dir, *vdir;
	struct dirent *dent, *vdent;
	char *p;
	int fd, ret;
	uint64_t coid;
	char vpath[1024];
	struct sheepdog_dir_entry *sde = req->data;

	dir = opendir(vdi_path);
	if (!dir)
		return SD_RES_NO_SUPER_OBJ;

	while ((dent = readdir(dir))) {
		if (!strcmp(dent->d_name, ".") ||
		    !strcmp(dent->d_name, ".."))
			continue;

		snprintf(vpath, sizeof(vpath), "%s%s", vdi_path, dent->d_name);

		fd = open(vpath, O_RDONLY);
		if (fd < 0) {
			eprintf("%m\n");
			return SD_RES_EIO;
		}

		ret = fgetxattr(fd, ANAME_CURRENT, &coid,
				sizeof(coid));
		if (ret != sizeof(coid)) {
			close(fd);
			return SD_RES_EIO;
		}

		dprintf("%lx\n", coid);

		close(fd);

		vdir = opendir(vpath);
		if (!vdir)
			return SD_RES_NO_VDI;

		while ((vdent = readdir(vdir))) {
			if (!strcmp(vdent->d_name, ".") ||
			    !strcmp(vdent->d_name, ".."))
				continue;

			p = strchr(vdent->d_name, '-');
			if (!p) {
				eprintf("bug %s\n", vdent->d_name);
				continue;
			}

			dprintf("%s\n", vdent->d_name);

			*p = '\0';

			sde->oid = strtoull(vdent->d_name, NULL, 16);
			sde->tag = strtoull(p + 1, NULL, 16);

			if (sde->oid == coid)
				sde->flags = FLAG_CURRENT;

			sde->name_len = strlen(dent->d_name);
			strcpy(sde->name, dent->d_name);
			sde = next_entry(sde);
		}
	}

	rsp->data_length = (char *)sde - (char *)req->data;
	dprintf("%d\n", rsp->data_length);

	return SD_RES_SUCCESS;
}

static int so_lookup_vdi(struct request *req)
{
	struct sd_so_req *hdr = (struct sd_so_req *)&req->rq;
	struct sd_so_rsp *rsp = (struct sd_so_rsp *)&req->rp;
	DIR *dir;
	struct dirent *dent;
	char *p;
	int fd, ret;
	uint64_t coid, oid;
	char path[1024];

	memset(path, 0, sizeof(path));
	snprintf(path, sizeof(path), "%s", vdi_path);
	strncpy(path + strlen(path), (char *)req->data,	hdr->data_length);

	dprintf("%s, %x\n", path, hdr->tag);

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		if (errno == ENOENT)
			return SD_RES_NO_VDI;
		else {
			eprintf("%m\n");
			return SD_RES_EIO;
		}
	}

	ret = fgetxattr(fd, ANAME_CURRENT, &coid,
			sizeof(coid));
	if (ret != sizeof(coid)) {
		close(fd);
		eprintf("%m\n");
		return SD_RES_EIO;
	}

	dprintf("%lx, %x\n", coid, hdr->tag);

	close(fd);

	if (hdr->tag == 0xffffffff) {
		close(fd);
		rsp->oid = coid;
		rsp->flags = SD_VDI_RSP_FLAG_CURRENT;
		return SD_RES_SUCCESS;
	}

	dir = opendir(path);
	if (!dir)
		return SD_RES_EIO;

	while ((dent = readdir(dir))) {
		if (!strcmp(dent->d_name, ".") ||
		    !strcmp(dent->d_name, ".."))
			continue;

		p = strchr(dent->d_name, '-');
		if (!p) {
			eprintf("bug %s\n", dent->d_name);
			continue;
		}

		if (strtoull(p + 1, NULL, 16) == hdr->tag) {
			*p = '\0';
			oid = strtoull(dent->d_name, NULL, 16);
			rsp->oid = oid;
			dprintf("%lx, %x\n", oid, hdr->tag);
			if (oid == coid)
				rsp->flags = SD_VDI_RSP_FLAG_CURRENT;

			ret = SD_RES_SUCCESS;
			break;
		}
	}
	closedir(dir);

	return SD_RES_SUCCESS;
}

void so_queue_request(struct work *work, int idx)
{
	struct request *req = container_of(work, struct request, work);
	struct sd_so_req *hdr = (struct sd_so_req *)&req->rq;
	struct sd_so_rsp *rsp = (struct sd_so_rsp *)&req->rp;
	struct cluster_info *cluster = req->ci->cluster;
	int nfd, fd = -1, ret, result = SD_RES_SUCCESS;
	uint32_t opcode = hdr->opcode;
	uint64_t last_oid = 0;
	char path[1024];

	if (list_empty(&cluster->node_list)) {
		/* we haven't got SD_OP_GET_NODE_LIST response yet. */
		result = SD_RES_SYSTEM_ERROR;
		goto out;
	}

	result = check_epoch(cluster, req);
	if (result != SD_RES_SUCCESS)
		goto out;

	memset(path, 0, sizeof(path));
	snprintf(path, sizeof(path), "%s", vdi_path);

	switch (opcode) {
	case SD_OP_SO:
		ret = mkdir(path, def_dmode);
		if (ret && errno != EEXIST) {
			result = SD_RES_EIO;
			goto out;
		}

		fd = open(path, O_RDONLY);
		if (fd < 0) {
			result = SD_RES_EIO;
			goto out;
		}

		ret = fsetxattr(fd, ANAME_LAST_OID, &last_oid,
				sizeof(last_oid), 0);
		if (ret) {
			close(fd);
			result = SD_RES_EIO;
			goto out;
		}

		ret = fsetxattr(fd, ANAME_COPIES, &hdr->copies,
				sizeof(hdr->copies), 0);
		if (ret)
			result = SD_RES_EIO;
		break;
	case SD_OP_SO_NEW_VDI:
		fd = open(path, O_RDONLY);
		if (fd < 0) {
			result = SD_RES_EIO;
			goto out;
		}

		ret = fgetxattr(fd, ANAME_LAST_OID, &last_oid,
				sizeof(last_oid));
		if (ret != sizeof(last_oid)) {
			close(fd);
			result = SD_RES_EIO;
			goto out;
		}

		strncpy(path + strlen(path), "/", 1);
		strncpy(path + strlen(path), (char *)req->data,	hdr->data_length);

		if (hdr->tag)
			;
		else {
			ret = mkdir(path, def_dmode);
			if (ret) {
				if (errno == EEXIST)
					result = SD_RES_VDI_EXIST;
				else {
					eprintf("%m\n");
					result = SD_RES_EIO;
				}
				goto out;
			}
		}

		nfd = open(path, O_RDONLY);
		if (nfd < 0) {
			eprintf("%m\n");
			result = SD_RES_EIO;
			goto out;
		}

		last_oid += MAX_DATA_OBJS;

		snprintf(path+ strlen(path), sizeof(path) - strlen(path),
			 "/%016lx-%08x", last_oid, hdr->tag);
		ret = creat(path, def_fmode);
		if (ret < 0) {
			eprintf("%m\n");
			result = SD_RES_EIO;
			goto out;
		}
		close(ret);

		ret = fsetxattr(fd, ANAME_LAST_OID, &last_oid,
				sizeof(last_oid), 0);
		if (ret) {
			eprintf("%m\n");
			close(fd);
			result = SD_RES_EIO;
			goto out;
		}

		close(fd);

		ret = fsetxattr(nfd, ANAME_CURRENT, &last_oid,
				sizeof(last_oid), 0);

		close(nfd);

		eprintf("%lx\n", last_oid);
		rsp->oid = last_oid;
		break;

	case SD_OP_SO_LOOKUP_VDI:
		result = so_lookup_vdi(req);
		break;
	case SD_OP_SO_READ_VDIS:
		result = so_read_vdis(req);
		break;
	case SD_OP_SO_STAT:
		fd = open(path, O_RDONLY);
		if (fd < 0) {
			eprintf("%m\n");
			result = SD_RES_EIO;
			goto out;
		}

		rsp->oid = 0;
		ret = fgetxattr(fd, ANAME_LAST_OID, &rsp->oid,
				sizeof(rsp->oid));
		if (ret != sizeof(rsp->oid)) {
			close(fd);
			result = SD_RES_SYSTEM_ERROR;
			goto out;
		}

		rsp->copies = 0;
		ret = fgetxattr(fd, ANAME_COPIES, &rsp->copies,
				sizeof(rsp->copies));
		if (ret != sizeof(rsp->copies)) {
			close(fd);
			result = SD_RES_SYSTEM_ERROR;
			goto out;
		}

		result = SD_RES_SUCCESS;
		break;
	}

out:
	if (result != SD_RES_SUCCESS)
		rsp->result = result;

	if (fd != -1)
		close(fd);
}

int epoch_log_write(uint32_t epoch, char *buf, int len)
{
	int fd, ret;
	char path[PATH_MAX];

	snprintf(path, sizeof(path), "%s%08u", epoch_path, epoch);
	fd = open(path, O_RDWR | O_CREAT |O_SYNC, def_fmode);
	if (fd < 0)
		return -1;

	ret = write(fd, buf, len);

	close(fd);

	if (ret != len)
		return -1;

	return 0;
}

int epoch_log_read(uint32_t epoch, char *buf, int len)
{
	int fd;
	char path[PATH_MAX];

	snprintf(path, sizeof(path), "%s%08u", epoch_path, epoch);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;

	len = read(fd, buf, len);

	close(fd);

	return len;
}

static int init_path(char *d, int *new)
{
	int ret, retry = 0;
	struct stat s;
again:
	ret = stat(d, &s);
	if (ret) {
		if (retry || errno != ENOENT) {
			eprintf("can't handle the dir %s, %m\n", d);
			return 1;
		}

		ret = mkdir(d, def_dmode);
		if (ret) {
			eprintf("can't create the dir %s, %m\n", d);
			return 1;
		} else {
			*new = 1;
			retry++;
			goto again;
		}
	}

	if (!S_ISDIR(s.st_mode)) {
		eprintf("%s is not a directory\n", d);
		return 1;
	}

	return 0;
}

static int init_base_path(char *d, int *new)
{
	return init_path(d, new);
}

#define VDI_PATH "/vdi/"

static int init_vdi_path(char *base_path, int new)
{
	int ret;
	struct stat s;

	vdi_path = zalloc(strlen(base_path) + strlen(VDI_PATH) + 1);
	sprintf(vdi_path, "%s" VDI_PATH, base_path);

	ret = stat(vdi_path, &s);
	if (ret) {
		if (errno != ENOENT)
			return 0;
	} else if (!new) {
		int fd, copies = 0;

		/* we need to recover the super object here. */

		fd = open(vdi_path, O_RDONLY);
		if (fd < 0)
			return 1;

		ret = fgetxattr(fd, ANAME_COPIES, &copies, sizeof(copies));

		close(fd);

		if (ret != sizeof(copies))
			return 1;

		nr_sobjs = copies;
	}

	return 0;
}

#define OBJ_PATH "/obj/"

static int init_obj_path(char *base_path)
{
	int new;

	obj_path = zalloc(strlen(base_path) + strlen(OBJ_PATH) + 1);
	sprintf(obj_path, "%s" OBJ_PATH, base_path);

	return init_path(obj_path, &new);
}

#define EPOCH_PATH "/epoch/"

static int init_epoch_path(char *base_path)
{
	int new;

	epoch_path = zalloc(strlen(base_path) + strlen(EPOCH_PATH) + 1);
	sprintf(epoch_path, "%s" EPOCH_PATH, base_path);

	return init_path(epoch_path, &new);
}

static int init_mnt_path(char *base_path)
{
	int ret;
	FILE *fp;
	struct mntent *mnt;
	struct stat s, ms;

	ret = stat(base_path, &s);
	if (ret)
		return 1;

	fp = setmntent(MOUNTED, "r");
	if (!fp)
		return 1;

	while ((mnt = getmntent(fp))) {
		ret = stat(mnt->mnt_dir, &ms);
		if (ret)
			continue;

		if (ms.st_dev == s.st_dev) {
			mnt_path = strdup(mnt->mnt_dir);
			break;
		}
	}

	endmntent(fp);

	return 0;
}

int init_store(char *d)
{
	int ret, new = 0;

	ret = init_base_path(d, &new);
	if (ret)
		return ret;

	ret = init_vdi_path(d, new);
	if (ret)
		return ret;

	ret = init_obj_path(d);
	if (ret)
		return ret;

	ret = init_epoch_path(d);
	if (ret)
		return ret;

	ret = init_mnt_path(d);
	if (ret)
		return ret;

	zero_block = zalloc(SD_DATA_OBJ_SIZE * DATA_OBJ_NR_WORKER_THREAD);
	if (!zero_block)
		return 1;

	return ret;
}
