FROM python:3.11-slim
RUN pip install --no-cache-dir pymysql pyarrow boto3
COPY archive.py /app/archive.py
WORKDIR /app
CMD ["python3", "archive.py"]
