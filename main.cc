#include "app.h"

uint8_t jpg_buf[1920*1080*4];
uint8_t jpg_eo_buf[1920*1080*3];
uint8_t jpg_ir_buf[1920*1080];

pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;

// Your code must be between pthread_mutex_lock() and status=true.
void *camera_loop(void *arg)
{
	int ret;
	ssize_t size, cap_size;
	daytime_ctx_t *ctx = (daytime_ctx_t*)arg;

	struct timeval start, end;
	double diffTime;

	size = ctx->eo.width * ctx->eo.height;

	ret = camera_streamon(ctx->eo.fd, 3);

	while (1) {

		ret = camera_get_frame_helper(ctx->eo.fd, &(ctx->eo.buf), &cap_size);
		rknn_run_helper(ctx->fus.ctx, ctx->eo.buf, size, ctx->fus.buf);
		memcpy(&(ctx->fus.buf[size]), &(ctx->eo.buf[size]), (size)/2);

		pthread_cond_signal(&cond);
	}
}

// About using condition variable
// https://stackoverflow.com/questions/16522858/understanding-of-pthread-cond-wait-and-pthread-cond-signal/16524148#16524148
// spurious wakeups
// https://stackoverflow.com/questions/8594591/why-does-pthread-cond-wait-have-spurious-wakeups

void *display_loop(void *arg)
{
	int ret;
	ssize_t size;
	rga_transform_t src, dst;
	struct timeval start, end;
	double diffTime;
	int crop_y;
	daytime_ctx_t *ctx = (daytime_ctx_t*)arg;
	drm_dev_t *mode = ctx->disp.list;

	CLEAR(src); CLEAR(dst);

	size = ctx->eo.width * ctx->eo.height;

	/*
	struct timeval start, end;
	double diffTime;

	gettimeofday(&start, NULL);
	gettimeofday(&end, NULL);
	diffTime = (end.tv_sec - start.tv_sec) * 1000.0;
	diffTime += (end.tv_usec - start.tv_usec) / 1000.0;
	printf("Display done : %f\n", diffTime);
*/
	
	while (1) {
		pthread_cond_wait(&cond, &mtx);
		usb_host_transfer(&ctx->usb.ctx, ctx->usb.buf, 307200, 0);

		// IR Up-Scaling
		src = {	.data = ctx->usb.buf, .width = (int)ctx->ir.height, .height = (int)ctx->ir.width, 
					.format = RK_FORMAT_YCbCr_420_P, .direction = 0 };
		dst = {	.data = ctx->ir.buf, .width = (int)ctx->eo.height, .height = (int)ctx->eo.width, 
					.format = RK_FORMAT_YCbCr_420_P, .direction = 0 };
		rga_transform(&src, &dst);

		// YCbCr Gray
		memset(ctx->tmpbuf, 128, size*2);

		// Legacy Fusion
		pthread_mutex_lock(&ctx->mutex_lock);
		fusion_legacy(ctx->fus.buf, ctx->ir.buf, ctx->tmpbuf,
							ctx->eo.width, ctx->eo.height, ctx->fus.crop_y, 5);
		pthread_mutex_unlock(&ctx->mutex_lock);


		// Display Fusion Image
		src = {	.data = ctx->tmpbuf, 
					.width = (int)(ctx->eo.height - ctx->fus.crop_y),
					.height = (int)ctx->eo.width, 
					.format = RK_FORMAT_YCbCr_420_P, 
					.direction = 0 };
		dst = {	.data = mode->bufs[mode->front_buf ^ 1].map, 
					.width = (int)ctx->disp.height, 
					.height = (int)ctx->disp.width, 
					.format = RK_FORMAT_BGRA_8888, 
					.direction = 0 };

		rga_transform(&src, &dst);
		drm_flip(mode);

//		memcpy(ctx->disp.list->map, jpg_buf, 1920*1080*4);
		
	}
}

void *usb_loop(void *arg)
{
	int ret;
	size_t size;
	daytime_ctx_t *ctx = (daytime_ctx_t*)arg;

//	size = ctx->ir.width * ctx->ir.height;
	size = ctx->eo.width * ctx->eo.height;

	while(1) {
//		usb_host_transfer(&ctx->usb.ctx, ctx->usb.buf, 307200, 0);
	}

}

void *icd_loop(void *arg)
{
	fd_set rfds;
	int ret, fd_max;
	uint8_t tmp[100];
	ssize_t bytes = 0;
	daytime_ctx_t *ctx = (daytime_ctx_t*)arg;

	fd_max = MAX(ctx->eo.uart.fd, ctx->ir.uart.fd);
	fd_max = MAX(fd_max, ctx->icd.fd);

	while(1)
	{
		// Add uart fd to check list
		FD_ZERO(&rfds);
		FD_SET(ctx->icd.fd, &rfds);
		FD_SET(ctx->eo.uart.fd, &rfds);
		FD_SET(ctx->ir.uart.fd, &rfds);

		select(fd_max + 1, &rfds, NULL, NULL, NULL);

		if (FD_ISSET(ctx->icd.fd, &rfds)) {
			bytes = read(ctx->icd.fd, tmp, 100);
			if (bytes > 0) {
				ret = mq_send(ctx->icd.mfd, (const char*)tmp, 100, 1);
				if (ret != 0) {
					printf("Failed to send message queue\n");
				}
				pthread_cond_signal(&ctx->thread_cond);
			}		
		}
		else if (FD_ISSET(ctx->eo.uart.fd, &rfds)) {
			uint32_t ack = (uint32_t)0xA1B2C3FE;
			bytes = read(ctx->eo.uart.fd, tmp, 100);
			if (bytes > 0) {
				printf("EO READ : ");
				for (int i = 0; i < bytes; i++)
					printf("0x%02X ", tmp[i]);
				printf("\n");

				write(ctx->eo.uart.fd, tmp, 100);
			}

		}
		else if (FD_ISSET(ctx->ir.uart.fd, &rfds)) {
			bytes = read(ctx->ir.uart.fd, tmp, 100);
			if (bytes > 0) {
				printf("IR READ : ");
				for (int i = 0; i < bytes; i++)
					printf("0x%02X ", tmp[i]);
				printf("\n");
			}
		}

	}

}

void *cmd_loop(void *arg)
{
	int ret, idx = 0;
	uint8_t tmp[100];
	char filename[50];
	uint16_t eo_y, ir_y;
	daytime_ctx_t *ctx = (daytime_ctx_t*)arg;

	while(1)
	{
		pthread_cond_wait(&ctx->thread_cond, &ctx->mutex_lock);
		
		ret = mq_receive(ctx->icd.mfd, (char *)tmp, 100, NULL);
		if (ret == -1) {
			perror("Failed to recv message queue ");
		}

		int mode_num = check_cmd(tmp, 100);
		if (tmp[5] == REGISTRATION_Y) {
			eo_y = (uint16_t) ((tmp[6] << 8) | tmp[7]);
			
			if (eo_y > 200)
				eo_y = 200;

			if (eo_y % 2 == 1)
				eo_y += 1;

			ctx->fus.crop_y = eo_y;
			printf("Change Y : %d\n", eo_y);
		}
		else if (tmp[5] == DATA_LOGGING && tmp[6] == 0x01) {
			if(mode_num==0)
			{
				memcpy(jpg_buf, ctx->disp.list->bufs[ctx->disp.list->front_buf ^ 1].map, 1920*1080*4);
				printf("Image save start__###################\n");
				sprintf(filename, "/sdcard/FUSION-%d.jpg", idx++);
				write_jpeg(filename, jpg_buf,
							1920, 1080, 4, TJPF_BGRA, 75);
			}
			else if(mode_num==1)
			{
				printf("\neo memcpy start\n");
				memcpy(jpg_eo_buf, ctx->eo.buf,1920*1080*3);
				printf("eo memcpy end\n");
				printf("Image save start__###################\n");
				sprintf(filename, "/sdcard/VISIBLE-%d.jpg", idx++);
				write_jpeg(filename, jpg_eo_buf,
							1920, 1080, 3, TJPF_BGRA, 75);
			}
			else if(mode_num==2)
			{
				printf("\nir memcpy start\n");
				memcpy(jpg_ir_buf, ctx->ir.buf, 1920*1080);
				printf("ir memcpy end\n");
				printf("Image save start__###################\n");
				sprintf(filename, "/sdcard/THERMAL-%d.jpg", idx++);
				write_jpeg(filename, jpg_ir_buf,
							1920, 1080, 1, TJPF_BGRA, 75);
			}
				
			for(int index=0; index<10; index++)
			{
				printf("tmp[%d] = %#x \n",index,tmp[index]);
			}
			printf("mode_num = %d\n",mode_num);
			printf("DATA_LOGGING = %#x \n",tmp[5]);
			printf("Image saved###test version\n");
		}

	}
}

int main(int argc, char** argv)
{
	int ret;
	ir_pkt pkt;
	daytime_ctx_t *app_ctx;
	ret = app_init(&app_ctx, "./config.cfg");

	pkt = (ir_pkt) {
				.sync = 0xFF, .address = 0x00,
				.cmd = 0x00, .cmd2 = 0x5E,
				.data = 0x00, .data2 = 0x01,
				.checksum = (0x00 + 0x00 + 0x5E + 0x00 + 0x01)
			};
//	write(app_ctx->ir.uart.fd, &pkt, sizeof(ir_pkt));

	ret = pthread_create(&app_ctx->cam_thread, NULL, camera_loop, (void*)app_ctx);
	if (ret < 0) {
		APP_ERR("Failed to create thread\n");
		exit(EXIT_FAILURE);
	}

	ret = pthread_create(&app_ctx->disp_thread, NULL, display_loop, (void*)app_ctx);
	if (ret < 0) {
		APP_ERR("Failed to create thread\n");
		exit(EXIT_FAILURE);
	}

	ret = pthread_create(&app_ctx->usb_thread, NULL, usb_loop, (void*)app_ctx);
	if (ret < 0) {
		APP_ERR("Failed to create thread\n");
		exit(EXIT_FAILURE);
	}

	ret = pthread_create(&app_ctx->icd_thread, NULL, icd_loop, (void*)app_ctx);
	if (ret < 0) {
		APP_ERR("Failed to create thread\n");
		exit(EXIT_FAILURE);
	}

	ret = pthread_create(&app_ctx->cmd_thread, NULL, cmd_loop, (void*)app_ctx);
	if (ret < 0) {
		APP_ERR("Failed to create thread\n");
		exit(EXIT_FAILURE);
	}
  
	ret = pthread_join(app_ctx->cam_thread, NULL);

	exit(EXIT_SUCCESS);

	return 0;
}

