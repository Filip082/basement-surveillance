
import { useEffect, useState } from 'react'
import './App.css'
import {
  ResponsiveContainer,
  LineChart,
  Line,
  XAxis,
  YAxis,
  CartesianGrid,
  Tooltip
} from 'recharts'

function App() {
  const [images, setImages] = useState([])
  const [loading, setLoading] = useState(true)
  const [error, setError] = useState(null)
  const [selectedImage, setSelectedImage] = useState(null)
  const [batteries, setBatteries] = useState({})
  const [batteryError, setBatteryError] = useState(null)
  const [showBatteryDetails, setShowBatteryDetails] = useState(false)
  const [batteryHistory, setBatteryHistory] = useState({})
  const [historyDays, setHistoryDays] = useState(7)
  const [historyError, setHistoryError] = useState(null)
  const [storage, setStorage] = useState(null)
  const [storageError, setStorageError] = useState(null)
  const [newStorageLimit, setNewStorageLimit] = useState(16)
  const [showStorageConfirm, setShowStorageConfirm] = useState(false)
  const [storageSaving, setStorageSaving] = useState(false)

  const [viewMode, setViewMode] = useState('list')
  const [pageSize, setPageSize] = useState(10)
  const [currentPage, setCurrentPage] = useState(1)

  const selectedImageIndex = images.findIndex(
    (image) => image.id === selectedImage?.id
  )

  const previousImage = () => {
    if (selectedImageIndex > 0) {
      setSelectedImage(images[selectedImageIndex - 1])
    }
  }

  const nextImage = () => {
    if (selectedImageIndex < images.length - 1) {
      setSelectedImage(images[selectedImageIndex + 1])
    }
  }
  useEffect(() => {
  const handleKeyDown = (event) => {
    if (!selectedImage) return

    if (event.key === 'Escape') {
      setSelectedImage(null)
    }

    if (event.key === 'ArrowLeft') {
      previousImage()
    }

    if (event.key === 'ArrowRight') {
      nextImage()
    }
  }

  window.addEventListener('keydown', handleKeyDown)

  return () => {
    window.removeEventListener('keydown', handleKeyDown)
  }
}, [selectedImage, selectedImageIndex])

  useEffect(() => {
    async function fetchStorage() {
      try {
        const response = await fetch('/api/storage')

        if (!response.ok) {
          throw new Error('Nie udało się pobrać informacji o pamięci')
        }

        const data = await response.json()
        setStorage(data)
        setNewStorageLimit(data.limitGB)
        setStorageError(null)
      } catch (error) {
        console.error('Storage loading failed:', error)
        setStorageError(error.message)
      }
    }

    fetchStorage()
  }, [])

    async function saveStorageLimit() {
    setStorageSaving(true)

    try {
      const response = await fetch('/api/storage/limit', {
        method: 'PUT',
        headers: {
          'Content-Type': 'application/json'
        },
        body: JSON.stringify({ limitGB: newStorageLimit })
      })

      const data = await response.json()

      if (!response.ok) {
        throw new Error(data.message || 'Nie udało się zmienić limitu')
      }

      setStorage({
        ...data,
        sizeGB: Number((data.sizeBytes / (1024 ** 3)).toFixed(3))
      })
      setStorageError(null)
      setShowStorageConfirm(false)
    } catch (error) {
      console.error('Storage limit update failed:', error)
      setStorageError(error.message)
      setShowStorageConfirm(false)
    } finally {
      setStorageSaving(false)
    }
  }
  useEffect(() => {
    async function fetchImages() {
      try {
        const response = await fetch('/api/images')

        if (!response.ok) {
          throw new Error('Nie udało się pobrać zdjęć')
        }

        const data = await response.json()
        setImages(data.images)

      } catch (error) {
        console.error('Image loading failed:', error)
        setError(error.message)

      } finally {
        setLoading(false)
      }
    }

    fetchImages()
  }, [])

  useEffect(() => {
    async function fetchBattery() {
      try {
        const response = await fetch('/api/telemetry')

        if (!response.ok) {
          throw new Error('Nie udało się pobrać stanu baterii')
        }

        const data = await response.json()
        setBatteries(data.battery || {})
        setBatteryError(null)

      } catch (error) {
        console.error('Battery loading failed:', error)
        setBatteryError(error.message)
      }
    }

    fetchBattery()

  }, [])

  useEffect(() => {
  if (!showBatteryDetails) return

  async function fetchBatteryHistory() {
    try {
      const response = await fetch(
        `/api/telemetry/history?days=${historyDays}`
      )

      if (!response.ok) {
        throw new Error('Nie udało się pobrać historii baterii')
      }

      const data = await response.json()

      setBatteryHistory(data.history || {})
      setHistoryError(null)
    } catch (error) {
      console.error('Battery history loading failed:', error)
      setHistoryError(error.message)
    }
  }

  fetchBatteryHistory()
}, [showBatteryDetails, historyDays])

  const totalPages = Math.ceil(images.length / pageSize)

  const startIndex = (currentPage - 1) * pageSize
  const endIndex = Math.min(startIndex + pageSize, images.length)

  const visibleImages = images.slice(startIndex, endIndex)

  if (loading) {
    return <p>Ładowanie zdjęć...</p>
  }

  if (error) {
    return <p>Błąd: {error}</p>
  }

  return (
    <main>
    <div className="dashboard-top">
      <div className="dashboard-header">
        <h1>System kontroli bezpieczeństwa</h1>
      </div>
            <section className="storage-section">
        <h2>Pamięć zdjęć</h2>

        {storageError ? (
          <p className="storage-error">Błąd odczytu pamięci</p>
        ) : storage === null ? (
          <p>Ładowanie informacji o pamięci...</p>
        ) : (
          <>
            <p>
              Zajętość: {storage.sizeGB} GB z {storage.limitGB} GB
            </p>

            <div className="storage-progress">
              <div
                className="storage-progress-fill"
                style={{
                  width: `${Math.min(storage.usagePercent, 100)}%`
                }}
              />
            </div>

            <p>{storage.usagePercent}% wykorzystanego miejsca</p>
            <div className="storage-controls">
              <label htmlFor="storageLimit">Limit:</label>

              <select
                id="storageLimit"
                value={newStorageLimit}
                onChange={(event) => setNewStorageLimit(Number(event.target.value))}
              >
                {Array.from({ length: 16 }, (_, index) => index + 1).map((value) => (
                  <option key={value} value={value}>
                    {value} GB
                  </option>
                ))}
              </select>

              <button
                type="button"
                onClick={() => setShowStorageConfirm(true)}
                disabled={newStorageLimit === storage.limitGB}
              >
                Zmień limit
              </button>
            </div>
          </>
        )}
      </section>

      <section
        className="devices-section"
        onClick={() => setShowBatteryDetails(true)}
      >
        <h2>Stan urządzeń</h2>

        {batteryError ? (
          <p className="battery-error">Błąd odczytu</p>
        ) : Object.keys(batteries).length === 0 ? (
          <p>Brak urządzeń</p>
        ) : (
          <div className="devices-grid">
            {Object.entries(batteries).map(([id, battery]) => (
              <div
                  className={`battery-card ${
                    battery > 50 ? 'battery-high' :
                    battery >= 25 ? 'battery-medium' :
                    'battery-low'
                  }`}
                  key={id}
                >
                <span className="device-name">{id}</span>
                <span className="battery-icon">🔋</span>
                <span className="battery-value">{battery}%</span>
              </div>
            ))}
          </div>
        )}

        <p className="devices-hint">
          Kliknij, aby zobaczyć historię
        </p>
      </section>
    </div>
      
      {showBatteryDetails && (
        <div
          className="battery-modal"
          onClick={() => setShowBatteryDetails(false)}
        >
          <section
            className="battery-modal-content"
            onClick={(event) => event.stopPropagation()}
          >
            <button
              type="button"
              className="battery-modal-close"
              onClick={() => setShowBatteryDetails(false)}
            >
              &times;
            </button>

            <h2>Historia baterii urządzeń</h2>

            <div className="history-controls">
              <label htmlFor="historyDays">Zakres historii:</label>

              <select
                id="historyDays"
                value={historyDays}
                onChange={(event) => setHistoryDays(Number(event.target.value))}
              >
                <option value={7}>7 dni</option>
                <option value={14}>14 dni</option>
                <option value={30}>30 dni</option>
              </select>
            </div>

            {historyError && (
              <p className="battery-error">{historyError}</p>
            )}

            {Object.entries(batteries).map(([id, battery]) => {
              const chartData = (batteryHistory[id] || []).map((entry) => ({
                battery: entry.battery,
                timestamp: new Date(entry.received_at).getTime()
              }))

              return (
                <div className="battery-detail-card" key={id}>
                  <h3>{id}</h3>
                  <p>Aktualny poziom baterii: {battery}%</p>

                  {chartData.length === 0 ? (
                    <p>Brak pomiarów w wybranym okresie.</p>
                  ) : (
                    <div className="battery-chart">
                      <ResponsiveContainer width="100%" height={280}>
                        <LineChart data={chartData}>
                          <CartesianGrid strokeDasharray="3 3" />
                          <XAxis
                            dataKey="timestamp"
                            type="number"
                            scale="time"
                            domain={['dataMin', 'dataMax']}
                            tickFormatter={(value) =>
                              new Date(value).toLocaleString('pl-PL')
                            }
                          />
                          <YAxis domain={[0, 100]} unit="%" />
                          <Tooltip
                            labelFormatter={(value) =>
                              new Date(value).toLocaleString('pl-PL')
                            }
                            formatter={(value) => [`${value}%`, 'Bateria']}
                          />
                          <Line
                            type="linear"
                            dataKey="battery"
                            stroke="#16a34a"
                            strokeWidth={2}
                            dot={{ r: 4 }}
                            activeDot={{ r: 6 }}
                            connectNulls={false}
                          />
                        </LineChart>
                      </ResponsiveContainer>
                    </div>
                  )}
                </div>
              )
            })}
          </section>
        </div>
      )}
      {showStorageConfirm && (
        <div
          className="storage-modal"
          onClick={() => {
            if (!storageSaving) setShowStorageConfirm(false)
          }}
        >
          <section
            className="storage-modal-content"
            onClick={(event) => event.stopPropagation()}
          >
            <h2>Potwierdzenie zmiany limitu</h2>

            <p>
              Czy na pewno chcesz zmienić limit pamięci z {storage.limitGB} GB
              na {newStorageLimit} GB?
            </p>

            {newStorageLimit < storage.limitGB && (
              <p className="storage-warning">
                Uwaga! Zmniejszenie limitu może spowodować automatyczne
                usunięcie najstarszych zdjęć wraz z ich wpisami w bazie danych.
              </p>
            )}

            <div className="storage-modal-actions">
              <button
                type="button"
                onClick={() => setShowStorageConfirm(false)}
                disabled={storageSaving}
              >
                Anuluj
              </button>

              <button
                type="button"
                onClick={saveStorageLimit}
                disabled={storageSaving}
              >
                {storageSaving ? 'Zapisywanie...' : 'Potwierdź zmianę'}
              </button>
            </div>
          </section>
        </div>
      )}
      <div className="gallery-toolbar">
        <div className="view-controls">
          <button
            type="button"
            className={viewMode === 'list' ? 'active' : ''}
            onClick={() => setViewMode('list')}
          >
            Lista
          </button>

          <button
            type="button"
            className={viewMode === 'grid' ? 'active' : ''}
            onClick={() => setViewMode('grid')}
          >
            Siatka
          </button>
        </div>

        <div className="page-size-control">
          <label htmlFor="pageSize">Zdjęć na stronie:</label>

          <select
            id="pageSize"
            value={pageSize}
            onChange={(event) => {
              setPageSize(Number(event.target.value))
              setCurrentPage(1)
            }}
          >
            <option value={5}>5</option>
            <option value={10}>10</option>
            <option value={20}>20</option>
            <option value={50}>50</option>
          </select>
        </div>
      </div>

      <p>
        Wyświetlono {images.length === 0 ? 0 : startIndex + 1}
        {'–'}
        {endIndex} z {images.length} zdjęć
      </p>

      {images.length === 0 ? (
        <p>Brak zapisanych zdjęć.</p>
      ) : (
        <div className={`gallery ${viewMode}`}>
          {visibleImages.map((image) => (
            <div className="gallery-item" key={image.id}>
              <img
                src={`/api/images/${image.id}/file`}
                alt={`Zdjęcie ${image.id}`}
                onClick={() => setSelectedImage(image)}
              />
              <div>
                <h3>
                  {new Date(image.event_timestamp).toLocaleString('pl-PL')}
                </h3>
                <p>Zdjęcie #{image.id}</p>
              </div>
            </div>
          ))}
        </div>
      )}

      {selectedImage && (
        <div
          className="image-modal"
          onClick={() => setSelectedImage(null)}
        >
          <div
            className="image-modal-content"
            onClick={(event) => event.stopPropagation()}
          >
            <button
              type="button"
              className="image-modal-close"
              onClick={() => setSelectedImage(null)}
            >
              &times;
            </button>

            <button
              type="button"
              className="image-modal-nav previous"
              onClick={previousImage}
              disabled={selectedImageIndex <= 0}
            >
              &#10094;
            </button>

            <button
              type="button"
              className="image-modal-nav next"
              onClick={nextImage}
              disabled={selectedImageIndex >= images.length - 1}
            >
              &#10095;
            </button>
            <img
              src={`/api/images/${selectedImage.id}/file`}
              alt={`Zdjęcie ${selectedImage.id}`}
            />

            <div className="image-modal-info">
              <h2>
                {new Date(selectedImage.event_timestamp).toLocaleString('pl-PL')}
              </h2>
              <p>Zdjęcie #{selectedImage.id}</p>
            </div>
          </div>
        </div>
      )}

      <div className="pagination">
        <button
          type="button"
          disabled={currentPage === 1}
          onClick={() => setCurrentPage((page) => page - 1)}
        >
          Poprzednia
        </button>

        <span>
          Strona {currentPage} z {Math.max(totalPages, 1)}
        </span>

        <button
          type="button"
          disabled={currentPage >= totalPages}
          onClick={() => setCurrentPage((page) => page + 1)}
        >
          Następna
        </button>
      </div>
    </main>
  )
}

export default App
