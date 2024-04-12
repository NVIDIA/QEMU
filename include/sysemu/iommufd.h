#ifndef SYSEMU_IOMMUFD_H
#define SYSEMU_IOMMUFD_H

#include "qom/object.h"
#include "exec/hwaddr.h"
#include "exec/cpu-common.h"
#include <linux/iommufd.h>
#include "sysemu/host_iommu_device.h"

#define TYPE_IOMMUFD_BACKEND "iommufd"
OBJECT_DECLARE_TYPE(IOMMUFDBackend, IOMMUFDBackendClass, IOMMUFD_BACKEND)

struct IOMMUFDBackendClass {
    ObjectClass parent_class;
};

struct IOMMUFDBackend {
    Object parent;

    /*< protected >*/
    int fd;            /* /dev/iommu file descriptor */
    bool owned;        /* is the /dev/iommu opened internally */
    uint32_t users;

    /*< public >*/
};

int iommufd_backend_connect(IOMMUFDBackend *be, Error **errp);
void iommufd_backend_disconnect(IOMMUFDBackend *be);

int iommufd_backend_alloc_ioas(IOMMUFDBackend *be, uint32_t *ioas_id,
                               Error **errp);
void iommufd_backend_free_id(IOMMUFDBackend *be, uint32_t id);
int iommufd_backend_map_dma(IOMMUFDBackend *be, uint32_t ioas_id, hwaddr iova,
                            ram_addr_t size, void *vaddr, bool readonly);
int iommufd_backend_unmap_dma(IOMMUFDBackend *be, uint32_t ioas_id,
                              hwaddr iova, ram_addr_t size);
int iommufd_backend_get_device_info(IOMMUFDBackend *be, uint32_t devid,
                                    enum iommu_hw_info_type *type,
                                    void *data, uint32_t len, Error **errp);
int iommufd_backend_alloc_hwpt(IOMMUFDBackend *be, uint32_t dev_id,
                               uint32_t pt_id, uint32_t flags,
                               uint32_t data_type, uint32_t data_len,
                               void *data_ptr, uint32_t *out_hwpt);
int iommufd_backend_invalidate_cache(IOMMUFDBackend *be, uint32_t hwpt_id,
                                     uint32_t data_type, uint32_t entry_len,
                                     uint32_t *entry_num, void *data_ptr);
int iommufd_backend_invalidate_dev_cache(IOMMUFDBackend *be, uint32_t dev_id,
                                         uint32_t data_type, uint32_t entry_len,
                                         uint32_t *entry_num, void *data_ptr);

typedef struct HIOD_IOMMUFD_INFO {
    enum iommu_hw_info_type type;
    union {
        struct iommu_hw_info_vtd vtd;
    } data;
} HIOD_IOMMUFD_INFO;

#define TYPE_HIOD_IOMMUFD TYPE_HOST_IOMMU_DEVICE "-iommufd"
OBJECT_DECLARE_TYPE(HIODIOMMUFD, HIODIOMMUFDClass, HIOD_IOMMUFD)

struct HIODIOMMUFD {
    /*< private >*/
    HostIOMMUDevice parent;

    /*< public >*/
    IOMMUFDBackend *iommufd;
    uint32_t devid;
    uint32_t ioas_id;
};

struct HIODIOMMUFDClass {
    /*< private >*/
    HostIOMMUDeviceClass parent_class;

    /*< public >*/
    /*
     * Attach/detach host IOMMU device to/from IOMMUFD hardware page
     * table, VFIO and VDPA device can have different implementation.
     */
    int (*attach_hwpt)(HIODIOMMUFD *idev, uint32_t hwpt_id, Error **errp);
    int (*detach_hwpt)(HIODIOMMUFD *idev, Error **errp);
};

void hiod_iommufd_init(HIODIOMMUFD *idev, IOMMUFDBackend *iommufd,
                       uint32_t devid, uint32_t ioas_id);
int hiod_iommufd_attach_hwpt(HIODIOMMUFD *idev, uint32_t hwpt_id,
                             Error **errp);
int hiod_iommufd_detach_hwpt(HIODIOMMUFD *idev, Error **errp);
#endif
