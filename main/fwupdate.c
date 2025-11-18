#include "esp_ota_ops.h"

#include "grbl/vfs.h"

#include "fwupdate.h"

typedef struct
{
    vfs_file_t  *handle;
    size_t      size;
    size_t      pos;
} file_t;

static file_t file;

static void file_close()
{
    if(file.handle) {
        vfs_close(file.handle);
        file.handle = 0;
    }
}

static bool file_open( char const *filename )
{
    if(file.handle) {
        file_close();
    }

    vfs_file_t *vft = vfs_open( filename, "r" );
    if (vft) {
        vfs_stat_t st;
        
        vfs_stat(filename, &st);
        
        file.handle = vft;
        file.size = st.st_size;
        file.pos = 0;
    }
    else {
        hal.stream.write("unable to open file\r\n");
    }

    return file.handle != 0;
    
}

static size_t file_read( uint8_t *pu8, size_t sMax )
{
    size_t sRead = vfs_read( pu8, 1, sMax, file.handle );
    if (sRead > 0) {
        file.pos = vfs_tell(file.handle);
    }
    return sRead;
}

#define READ_SIZE 8192

static status_code_t fwupdate_do_update(sys_state_t state, char *args)
{
    status_code_t ret = Status_FileOpenFailed;
    uint8_t *pu8 = 0;

    esp_err_t err;
    esp_ota_handle_t update_handle = 0;

    const esp_partition_t *update_partition = 0;

    const esp_partition_t *configured = esp_ota_get_boot_partition();
    const esp_partition_t *running = esp_ota_get_running_partition();

    if (configured != running) {
        char buf[256];

        hal.stream.write( "OTA Partitions are corrupt\r\n" );
//        snprintf( buf, sizeof(buf), "Boot %08x - running %08x\r\n", configured->address, running->address );
        snprintf( buf, sizeof(buf), "Boot %p - running %p\r\n", configured, running );
        hal.stream.write( buf );
        goto cleanup;
    }

    update_partition = esp_ota_get_next_update_partition(0);

    if (!update_partition) {
        hal.stream.write( "No available update partitions\r\n" );
        goto cleanup;
    }

    pu8 = malloc( READ_SIZE );
    if (!pu8) {
        hal.stream.write( "Out of memory\r\n" );
        goto cleanup;
    }
    if (file_open(args)) {

        bool image_header_was_checked = false;
        size_t written = 0;
        while (written < file.size) {
            size_t cnt = file_read( pu8, 8192 );

            if (cnt == 0 ) {
                hal.stream.write( "Failed reading from input file\r\n" );
                goto cleanup;
            }

            if (!image_header_was_checked) {
                esp_app_desc_t new_app_info;
                if( cnt > sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t)) {
                    memcpy(&new_app_info, &pu8[sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t)], sizeof(esp_app_desc_t));

                    hal.stream.write( "New Firmware version: " );
                    hal.stream.write( new_app_info.version );
                    hal.stream.write( "\r\n" );

                    image_header_was_checked = true;

                    err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &update_handle );
                    if (err != ESP_OK) {
                        hal.stream.write( "Failed to start update\r\n" );
                        esp_ota_abort(update_handle);
                        goto cleanup;
                    }
                }
                else {
                    hal.stream.write( "READ size too small\r\n" );
                    goto cleanup;
                }
            }

            err = esp_ota_write( update_handle, pu8, cnt );
            if (err != ESP_OK) {
                hal.stream.write( "Error during update\r\n" );
                esp_ota_abort( update_handle );
                goto cleanup;
            }
            written += cnt;
        }
        
        err = esp_ota_end( update_handle );
        if (err != ESP_OK) {
            if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
                hal.stream.write( "Image is corrupt\r\n" );
            }
            else {
                hal.stream.write( "esp_ota_end() failed\r\n" );
            }
            goto cleanup;
        }

        err = esp_ota_set_boot_partition(update_partition);
        if (err != ESP_OK) {
            hal.stream.write( "esp_oat_set_boot_parition() failed\r\n" );
            goto cleanup;
        }
        else {
            hal.stream.write( "Update Successful.  Please reset the system!\r\n" );
        }
    	ret =  Status_OK;
    }

cleanup:
    file_close();
    if (pu8) {
        free( pu8 );
    }

    return ret;
}

void fwupdate_init(void)
{
    PROGMEM static const sys_command_t fwupdate_command_list[] = {
        {"FUPDT", fwupdate_do_update, {}, { .str = "$FUPDT=file - update firmware from file." } },
    };

    static sys_commands_t fwupdate_commands = {
        .n_commands = sizeof(fwupdate_command_list) / sizeof(sys_command_t),
        .commands = fwupdate_command_list
    };

    PROGMEM static const status_detail_t status_detail[] = {
        { Status_FileOpenFailed, "Unable to open URL" }
    };

    static error_details_t error_details = {
        .errors = status_detail,
        .n_errors = sizeof(status_detail) / sizeof(status_detail_t)
    };

//    errors_register(&error_details);
    system_register_commands(&fwupdate_commands);
}

