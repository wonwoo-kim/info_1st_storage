#include "fusion.h"
int mode_num = -1;
int uart_fd;
uint8_t tmp[255];
uint8_t cmd_queue[QUEUE_SIZE];
uint8_t queue_index = 0;

int fusion_eo_ir(fus_ctx_t *ctx, float level, int flag)
{
	if (flag == 0) {
		fusion_npu(ctx, level);
	}
	else {
//		fusion_legacy(ctx, level);
	}

	printf("\n\n\n\n\n\n\n@%#@%@#%@#%@#%@fusion_eo_ir@#%@#%@#%@#%@#%@#%@#%\n\n\n\n\n\n\n");
}

int fusion_legacy(uint8_t *eo, uint8_t *ir, uint8_t *out, 
						size_t width, size_t height, size_t crop_y, uint8_t level)
{
	register int i, j;
	uint32_t offset, size, crop_sz;
	uint16_t eo_tmp, ir_tmp;

	offset = crop_y * width;
	size = width * height;
	crop_sz = size - offset;

	// Fusion Y Colorspace
#if 1
	uint8_t qtn, remain;		// quotient, remainder

	uint8x16_t eo_vec, ir_vec;
	uint16x8_t eo_l, eo_h, ir_l, ir_h, out_l, out_h;
	uint16x8_t eo_level, ir_level, mul_vec;

	eo_level = vdupq_n_u16(level);
	ir_level = vdupq_n_u16(10 - level);
	mul_vec = vdupq_n_u16(26);

	qtn = crop_sz / 16;
	remain = crop_sz % 16;

	/*
	vld1q_u8 : load uint8x16
	vmovn_u16 : uint16x8_t to uint8x8 (Extract narrow)
	
	*/
	for (i = offset; i < size-remain; i += 16) {

		// Load vector
		eo_vec = vld1q_u8(&eo[i]);
		ir_vec = vld1q_u8(&ir[i - offset]);

		// Separate high/low vector
		eo_l = vmovl_u8(vget_low_u8(eo_vec));
		eo_h = vmovl_u8(vget_high_u8(eo_vec));

		ir_l = vmovl_u8(vget_low_u8(ir_vec));
		ir_h = vmovl_u8(vget_high_u8(ir_vec));

		// Multiply by level
		eo_l = vmulq_u16(eo_l, eo_level);
		eo_h = vmulq_u16(eo_h, eo_level);

		ir_l = vmulq_u16(ir_l, ir_level);
		ir_h = vmulq_u16(ir_h, ir_level);

		// Multiply by 205 (for div10)
		eo_l = vmulq_u16(eo_l, mul_vec);
		eo_h = vmulq_u16(eo_h, mul_vec);
		ir_l = vmulq_u16(ir_l, mul_vec);
		ir_h = vmulq_u16(ir_h, mul_vec);

		// Shift right (>> 11)
		eo_l = vshrq_n_u16(eo_l, 8);
		eo_h = vshrq_n_u16(eo_h, 8);
		ir_l = vshrq_n_u16(ir_l, 8);
		ir_h = vshrq_n_u16(ir_h, 8);

		// Add
		out_l = vaddq_u16(eo_l, ir_l);
		out_h = vaddq_u16(eo_h, ir_h);

		// uint16x8_t => uint8x8_t
		vst1_u8(&out[i - offset], vmovn_u16(out_l));
		vst1_u8(&out[i+8 - offset], vmovn_u16(out_h));
	}

	for (i = size-remain; i < size; i ++) {
		eo_tmp = (((uint16_t)eo[i] * 5) * 26) >> 8;
		ir_tmp = (((uint16_t)ir[i - offset] * 5) * 26) >> 8;
		out[i - offset] = (uint8_t)(eo_tmp + ir_tmp);
	}
#else
	for (i = offset; i < size; i++) {
#if 1
		eo_tmp = (((uint16_t)eo[i] * 5) * 205) >> 11;
		ir_tmp = (((uint16_t)ir[i - offset] * 5) * 205) >> 11;
		out[i] = (uint8_t)(eo_tmp + ir_tmp);
#else
		out[i] = 
					(uint8_t)( (eo[offset + (i - offset)] * 0.5) 
					+ (ir[i - offset] * 0.5) );
#endif


	}
#endif

	/* Crop & Copy Cb/Cr Colorspace
	 *
	 *  +-----------------------+      
	 *  |     Y Colorspace      |      
	 *  |        (W * H)        |        
	 *  +-----------+-----------+
	 *  |     Cb    |    Cr     |
	 *  | (W * H)/4 | (W * H)/4 |
	 *  +-----------+-----------+
	 */
#if 1
	memcpy(&out[crop_sz], 
			&(eo[size + (offset/4)]), (crop_sz)/4);	//Cb
	memcpy(&out[crop_sz + (crop_sz/4)],
			&(eo[size + (size / 4) + (offset/4)]), (crop_sz)/4);	//Cr
#else
	memcpy(&(out[size + ((offset)/4)]), 
			&(eo[size + (offset/4)]), (crop_sz)/4);	//Cb
	memcpy(&(out[size + (size/4) + ((offset)/4)]),
			&(eo[size + (size / 4) + (offset/4)]), (crop_sz)/4);	//Cr
#endif
}

int fusion_npu(fus_ctx_t *ctx, float level)
{

	// Run Eo fusion model
	rknn_run_helper(ctx->fusion_model, ctx->eo, 
						(ctx->width*ctx->height), ctx->eo);

	printf("\n\n\n\n\n\n\n@%#@%#@%#@%#@%@%#@fusion_npu@%#@%#@%#@%#@%#@%#@%#@\n\n\n\n\n\n\n");
//	fusion_legacy(ctx, level);

}

#if 0
static inline void pack_uplink_cmd(uplink_cmt_t *cmd)
{
	memset(cmd, 0x00, sizeof(uplink_cmd_t));
	memcpy(cmd->header, UPLINK_HEADER, sizeof(UPLINK_HEADER));
}

static inline void pack_downlink_cmd(downlink_cmt_t *cmd)
{
	memset(cmd, 0x00, sizeof(downlink_cmd_t));
	memcpy(cmd->header, DOWNLINK_HEADER, sizeof(DOWNLINK_HEADER));
}
#endif

void icd_init(const char *device, speed_t baudrate)
{
	memset(cmd_queue, 0x00, sizeof(cmd_queue));
	uart_init_helper(device, baudrate, icd_callback, &uart_fd);
}

void icd_callback(int status)
{
	int ret;
	ssize_t bytes;

	bytes = read(uart_fd, tmp, 255);

	queue_write(tmp, bytes);
	check_cmd(cmd_queue, QUEUE_SIZE);
}

void icd_deinit(void)
{
	uart_deinit_helper(uart_fd);
}


void queue_write(void *buf, ssize_t bytes)
{
	void *tmp;
	int overflow = (queue_index + bytes) - QUEUE_SIZE;
	
	if (overflow > 0) {
		queue_index -= overflow;

		memmove(cmd_queue, &cmd_queue[overflow], queue_index);
		memcpy(&cmd_queue[queue_index], buf, bytes);

		queue_index += bytes;
	} 
	else {
		memcpy(&cmd_queue[queue_index], buf, bytes);
		queue_index += bytes;
	}

}

int check_cmd(uint8_t *queue, ssize_t size)
{
	const uint8_t HEADER[3] = {0xA1, 0xB2, 0x01};
	uplink_cmd_t cmd;

	void *offset;
	int i;

	offset = memmem(queue, size, HEADER, sizeof(HEADER));
	

	if (offset != NULL) {
		memcpy(&cmd, offset, sizeof(cmd));

		printf("//////////////// MESSAGE RECV ////////////////\n");
		printf("HEADER : \t\tOK\n");
		printf("MESSAGE : \t\t" );

		switch(cmd.msg.address[0]) 
		{
			case VIDEO_OUTPUT_MODE:
				printf("Select video output mode : ");
				switch(cmd.msg.data[0]) 
				{
					case FUSION_SELECT:
						printf("my FUSION");
						mode_num = 0;
						printf("<< mode_num = %d >>",mode_num); 
						break;
					case VISIBLE_SELECT:
						printf("my VISIBLE");
						mode_num = 1;
						printf("<< mode_num = %d >>",mode_num);
						break;
					case IR_SELECT:
						printf("my IR");
						mode_num = 2;
						printf("<< mode_num = %d >>",mode_num);
						break;
					default:
						break;
				}
				break;
			case VIDEO_OUTPUT_METHOD:
				printf("Select video output method : ");
				switch(cmd.msg.data[0])
				{
					case DIGITAL_OUTPUT:
						printf("DIGITAL");
						break;
					case ANALOG_OUTPUT:
						printf("ANALOG");
						break;
					case DIGITAL_ANALOG_OUTPUT:
						printf("DIGITAL/ANALOG");
						break;
					default:
						break;
				}
				break;
			case OSD_ENABLE:
				printf("Determine draw OSD : ");
				switch(cmd.msg.data[0])
				{
					case ON:
						printf("ON");
						break;
					case OFF:
						printf("OFF");
						break;
					default:
						break;
				}
				break;
			case DATA_LOGGING:
				printf("Data Logging : ");
				switch(cmd.msg.data[0])
				{
					case ON:
						printf("ON");
						break;
					case OFF:
						printf("OFF");
						break;
					default:
						break;
				}
				break;
			case REGISTRATION_Y:
				uint16_t eo_y, ir_y;
				eo_y = (uint16_t) ((cmd.msg.data[0] << 8) | cmd.msg.data[1]);
				ir_y = (uint16_t) ((cmd.msg.data[2] << 8) | cmd.msg.data[3]);
				printf("Set Registration Y (CCD : %u  IR : %u)", eo_y, ir_y);
				break;
			case REGISTRATION_X:
				printf("Set Registration X");
				break; 
			case REGISTRATION_SAVE:
				printf("Save registration to sd card");
				break;
			case READ_REGISTRATION_Y:
				printf("Read registration Y");
				break;
			case READ_REGISTRATION_X:
				printf("Read registration X");
				break;
			case FUSION_LEVEL_AUTO:
				printf("Determine set fusion auto : ");
				switch(cmd.msg.data[0])
				{
					case OFF:
						printf("OFF");
						break;
					case ON:
						printf("ON");
						break;
					default:
						break;
				}
				break;
			case SET_FUSION_LEVEL:
				printf("Set fusion level(manual)");
				break;
			case REQ_SELF_TEST:
				printf("Request BIT");
				break;
			case VERTICAL_FLIP:
				printf("Vertical flip : ");
				switch(cmd.msg.data[0])
				{
					case OFF:
						printf("OFF");
						break;
					case ON:
						printf("ON");
						break;
					default:
						break;
				}
				break;			
			case HORIZONTAL_FLIP:
				printf("Horizontal flip : ");
				switch(cmd.msg.data[0])
				{
					case OFF:
						printf("OFF");
						break;
					case ON:
						printf("ON");
						break;
					default:
						break;
				}
				break;
			case OUTPUT_RESOLUTION:
				printf("Select Output Resolution : ");
				switch(cmd.msg.data[0])
				{
					case FUSION_SELECT:
						printf("Fusion");
						break;
					case VISIBLE_SELECT:
						printf("Visible");
						break;
					case IR_SELECT:
						printf("IR");
						break;
					default:
						break;
				}
				break;
			default:
				break;
		}
		printf("\n");

		printf("TAIL : \t\t\t%02x %02x\n", cmd.tail[0], cmd.tail[1]);
		printf("//////////////////////////////////////////////\n\n\n");
	}
	return mode_num;
}


