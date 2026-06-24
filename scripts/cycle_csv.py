import os
import csv
import argparse
import sys
import time

argp = argparse.ArgumentParser(description="Generate Block_id & cycle csv")
argp.add_argument("-i", "--input", default=".", type=str)
args = argp.parse_args()

def parse_write(file_path, csv_init_name):
    max_block_num = 5000
    max_cycle = 100000
    matrix = [["" for _ in range(max_block_num)] for __ in range(max_cycle+1)]
    for row in sys.stdin:
        if (" BPC " in row and "B0 " not in row) or "Total Cycles" in row:
            matrix[int(cycle)][int(head_index)] = "BPC:" + bpc + " " + block_type
        
        if " BPC " in row:
            if "BSize 0" in row:
                cycle = 0
            # col index
            head_index = int(row.split(" ")[0][1:])
            bpc = row.split(" ")[2].strip()
            if "Block." in row.split(" ")[3].strip():
                block_type = row.split(" ")[3].strip().split(".")[1].strip()
            elif "Block" == row.split(" ")[3].strip():
                block_type = "STD"
            if head_index == (max_block_num):
                break

        elif "| BPC:" in row and "| TPC:" in row:
            row_after = row.split('|')
            # row index
            cycle = int(row_after[1].strip().split(':')[-1])
            inst = row_after[-1].strip()
            matrix[int(cycle)-1][int(head_index)] = inst
            if cycle == max_cycle:
                matrix[int(cycle)][int(head_index)] = "BPC:" + bpc + " " + block_type
                break

            

    with open(csv_init_name, 'w', encoding='UTF8') as file:
        writer = csv.writer(file)
        sum_parallel_num = 0
        sum_row = 0
        empty_row = 0
        for index, row in enumerate(matrix):
            del_str_num = 0
            for ele in row:
                if ele == '' or 'BPC:' in ele:
                    del_str_num += 1
            parallel_num = len(row) - del_str_num
            if parallel_num > 0:
                empty_row = 0
                writer.writerow(row)
                if index != 0:
                    sum_parallel_num += parallel_num
                    sum_row += 1
            else:
                empty_row += 1
                writer.writerow(row)
                if empty_row == 4: 
                    break
                
        ave_parallel_num =  round(sum_parallel_num / sum_row, 1)

    csv_name = file_path.replace(".json", "_" + str(ave_parallel_num) + ".csv")
    return csv_name


def main():
    file_path = args.input
    print(file_path)
    csv_init_name = file_path.replace(".json", ".csv")
    if file_path.endswith(".json"):
        csv_final_name = parse_write(file_path, csv_init_name)
        os.system("mv " + csv_init_name + " " + csv_final_name)

if __name__ == '__main__':
    main()
